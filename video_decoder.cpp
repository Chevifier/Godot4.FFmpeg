/**************************************************************************/
/*  video_decoder.cpp                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           EIRTeam.Steamworks                           */
/*                         https://ph.eirteam.moe                         */
/**************************************************************************/
/* Copyright (c) 2023-present Álex Román (EIRTeam) & contributors.        */
/*                                                                        */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "video_decoder.h"
#include "modules/ffmpeg/ffmpeg_frame.h"
#include "tracy_import.h"

extern "C" {
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
}

const int MAX_PENDING_FRAMES = 2;

bool is_hardware_pixel_format(AVPixelFormat p_fmt) {
	switch (p_fmt) {
		case AV_PIX_FMT_VDPAU:
		case AV_PIX_FMT_CUDA:
		case AV_PIX_FMT_VAAPI:
		case AV_PIX_FMT_DXVA2_VLD:
		case AV_PIX_FMT_QSV:
		case AV_PIX_FMT_VIDEOTOOLBOX:
		case AV_PIX_FMT_D3D11:
		case AV_PIX_FMT_D3D11VA_VLD:
		case AV_PIX_FMT_DRM_PRIME:
		case AV_PIX_FMT_OPENCL:
		case AV_PIX_FMT_MEDIACODEC:
		case AV_PIX_FMT_VULKAN:
		case AV_PIX_FMT_MMAL:
		case AV_PIX_FMT_XVMC: {
			return true;
		}
		default: {
			return false;
		}
	}
	return false;
}

String ffmpeg_get_error_message(int p_error_code) {
	const ulong buffer_size = 256;
	Vector<char> buffer;
	buffer.resize(buffer_size);

	int str_error_code = av_strerror(p_error_code, buffer.ptrw(), buffer.size());

	if (str_error_code < 0) {
		return vformat("%d (av_strerror failed with code %d)", p_error_code, str_error_code);
	}

	return String::utf8(buffer.ptr());
}

int VideoDecoder::_read_packet_callback(void *p_opaque, uint8_t *p_buf, int p_buf_size) {
	VideoDecoder *decoder = (VideoDecoder *)p_opaque;
	uint64_t read_bytes = decoder->video_stream->get_buffer(p_buf, p_buf_size);
	return read_bytes != 0 ? read_bytes : AVERROR_EOF;
}

int64_t VideoDecoder::_stream_seek_callback(void *p_opaque, int64_t p_offset, int p_whence) {
	VideoDecoder *decoder = (VideoDecoder *)p_opaque;
	switch (p_whence) {
		case SEEK_CUR: {
			decoder->video_stream->seek(decoder->video_stream->get_position() + p_offset);
		} break;
		case SEEK_SET: {
			decoder->video_stream->seek(p_offset);
		} break;
		case SEEK_END: {
			decoder->video_stream->seek_end(p_offset);
		} break;
		case AVSEEK_SIZE: {
			return decoder->video_stream->get_length();
		} break;
		default: {
			return -1;
		} break;
	}
	return decoder->video_stream->get_position();
}

void VideoDecoder::prepare_decoding() {
	const int context_buffer_size = 4096;
	unsigned char *context_buffer = (unsigned char *)av_malloc(context_buffer_size);
	io_context = avio_alloc_context(context_buffer, context_buffer_size, 0, this, &VideoDecoder::_read_packet_callback, nullptr, &VideoDecoder::_stream_seek_callback);

	format_context = avformat_alloc_context();
	format_context->pb = io_context;
	format_context->flags |= AVFMT_FLAG_GENPTS; // required for most HW decoders as they only read `pts`

	int open_input_res = avformat_open_input(&format_context, "dummy", nullptr, nullptr);
	input_opened = open_input_res >= 0;
	ERR_FAIL_COND_MSG(!input_opened, vformat("Error opening file or stream: %s", ffmpeg_get_error_message(open_input_res)));

	int find_stream_info_result = avformat_find_stream_info(format_context, nullptr);
	ERR_FAIL_COND_MSG(find_stream_info_result < 0, vformat("Error finding stream info: %s", ffmpeg_get_error_message(find_stream_info_result)));

	int stream_index = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	ERR_FAIL_COND_MSG(stream_index < 0, vformat("Couldn't find video stream: %s", ffmpeg_get_error_message(stream_index)));

	stream = format_context->streams[stream_index];
	time_base_in_seconds = stream->time_base.num / (double)stream->time_base.den;
	if (stream->duration > 0) {
		duration = stream->duration * time_base_in_seconds * 1000.0;
	} else {
		duration = format_context->duration / (double)AV_TIME_BASE * 1000.0;
	}
}

void VideoDecoder::recreate_codec_context() {
	if (stream == nullptr) {
		return;
	}

	AVCodecParameters codec_params = *stream->codecpar;
	BitField<HardwareVideoDecoder> target_hw_decoders = hw_decoding_allowed ? target_hw_video_decoders : HardwareVideoDecoder::NONE;
	bool open_successful = false;

	for (const AvailableDecoderInfo &info : get_available_decoders(format_context->iformat, codec_params.codec_id, target_hw_decoders)) {
		if (codec_context != nullptr) {
			avcodec_free_context(&codec_context);
		}
		codec_context = avcodec_alloc_context3(info.codec->get_codec_ptr());
		codec_context->pkt_timebase = stream->time_base;

		ERR_CONTINUE_MSG(codec_context == nullptr, vformat("Couldn't allocate codec context: %s", info.codec->get_codec_ptr()->name));

		int param_copy_result = avcodec_parameters_to_context(codec_context, &codec_params);

		ERR_CONTINUE_MSG(param_copy_result < 0, vformat("Couldn't copy codec parameters from %s: %s", info.codec->get_codec_ptr()->name, ffmpeg_get_error_message(param_copy_result)));

		// Try to init hw decode context
		if (info.device_type != AV_HWDEVICE_TYPE_NONE) {
			int hw_device_create_result = av_hwdevice_ctx_create(&codec_context->hw_device_ctx, info.device_type, nullptr, nullptr, 0);
			ERR_CONTINUE_MSG(hw_device_create_result < 0, vformat("Couldn't create hardware video decoder context %s for codec %s: %s", av_hwdevice_get_type_name(info.device_type), info.codec->get_codec_ptr()->name, ffmpeg_get_error_message(hw_device_create_result)));

			print_line(vformat("Succesfully opened hardware video decoder context %s for codec %s", av_hwdevice_get_type_name(info.device_type), info.codec->get_codec_ptr()->name));
		} else {
			codec_context->thread_count = 0;
		}

		int open_codec_result = avcodec_open2(codec_context, info.codec->get_codec_ptr(), nullptr);
		ERR_CONTINUE_MSG(open_codec_result < 0, vformat("Error trying to open %s codec: %s", info.codec->get_codec_ptr()->name, ffmpeg_get_error_message(open_codec_result)));

		print_line("Succesfully initialized decoder:", info.codec->get_codec_ptr()->name);
		open_successful = true;
		break;
	}
}

VideoDecoder::HardwareVideoDecoder VideoDecoder::from_av_hw_device_type(AVHWDeviceType p_device_type) {
	switch (p_device_type) {
		case AV_HWDEVICE_TYPE_NONE: {
			return VideoDecoder::NONE;
		} break;
		case AV_HWDEVICE_TYPE_VDPAU: {
			return VideoDecoder::VDPAU;
		} break;
		case AV_HWDEVICE_TYPE_CUDA: {
			return VideoDecoder::NVDEC;
		} break;
		case AV_HWDEVICE_TYPE_VAAPI: {
			return VideoDecoder::VAAPI;
		} break;
		case AV_HWDEVICE_TYPE_DXVA2: {
			return VideoDecoder::DXVA2;
		} break;
		case AV_HWDEVICE_TYPE_QSV: {
			return VideoDecoder::INTEL_QUICK_SYNC;
		} break;
		case AV_HWDEVICE_TYPE_MEDIACODEC: {
			return VideoDecoder::ANDROID_MEDIACODEC;
		} break;
		default: {
		} break;
	}
	return VideoDecoder::NONE;
}

void VideoDecoder::_seek_command(double p_target_timestamp) {
	avcodec_flush_buffers(codec_context);
	av_seek_frame(format_context, stream->index, (long)(p_target_timestamp / time_base_in_seconds / 1000.0), AVSEEK_FLAG_BACKWARD);
	skip_output_until_time = p_target_timestamp;
	decoder_state = DecoderState::READY;
}

const char *const decoder_loop = "Video decoding";

void VideoDecoder::_thread_func(void *userdata) {
	VideoDecoder *decoder = (VideoDecoder *)userdata;
	AVPacket *packet = av_packet_alloc();
	AVFrame *receive_frame = av_frame_alloc();

	String video_decoding_str = vformat("Video decoding %d", Thread::get_caller_id());
	CharString str = video_decoding_str.utf8();
	const char *const video_decoding = str.ptr();
	while (!decoder->thread_abort.is_set()) {
		switch (decoder->decoder_state) {
			case READY:
			case RUNNING: {
				decoder->decoded_frames_mutex.lock();
				bool needs_frame = decoder->decoded_frames.size() < MAX_PENDING_FRAMES;
				decoder->decoded_frames_mutex.unlock();
				if (needs_frame) {
					FrameMarkStart(video_decoding);
					decoder->_decode_next_frame(packet, receive_frame);
					FrameMarkEnd(video_decoding);
				} else {
					decoder->decoder_state = DecoderState::READY;
					OS::get_singleton()->delay_usec(1000);
				}
			} break;
			case END_OF_STREAM: {
				// While at the end of the stream, avoid attempting to read further as this comes with a non-negligible overhead.
				// A Seek() operation will trigger a state change, allowing decoding to potentially start again.
				OS::get_singleton()->delay_usec(50000);
			} break;
			default: {
				ERR_PRINT("Invalid decoder state");
			} break;
		}
		decoder->decoder_commands.flush_if_pending();
	}

	av_packet_free(&packet);
	av_frame_free(&receive_frame);

	if (decoder->decoder_state != DecoderState::FAULTED) {
		decoder->decoder_state = DecoderState::STOPPED;
	}
}

void VideoDecoder::_decode_next_frame(AVPacket *p_packet, AVFrame *p_receive_frame) {
	ZoneScopedN("Video decoder decode next frame");
	int read_frame_result = 0;

	if (p_packet->buf == nullptr) {
		read_frame_result = av_read_frame(format_context, p_packet);
	}

	if (read_frame_result >= 0) {
		decoder_state = DecoderState::RUNNING;

		bool unref_packet = true;

		if (p_packet->stream_index == stream->index) {
			int send_packet_result = _send_packet(p_receive_frame, p_packet);

			if (send_packet_result == -EAGAIN) {
				unref_packet = false;
			}
		}

		if (unref_packet) {
			av_packet_unref(p_packet);
		}
	} else if (read_frame_result == AVERROR_EOF) {
		_send_packet(p_receive_frame, nullptr);
		if (looping) {
			seek(0);
		} else {
			decoder_state = DecoderState::END_OF_STREAM;
		}
	} else if (read_frame_result == -EAGAIN) {
		decoder_state = DecoderState::READY;
		OS::get_singleton()->delay_usec(1000);
	} else {
		print_line(vformat("Failed to read data into avcodec packet: %s", ffmpeg_get_error_message(read_frame_result)));
	}
}

int VideoDecoder::_send_packet(AVFrame *p_receive_frame, AVPacket *p_packet) {
	ZoneScopedN("Video decoder send packet");
	// send the packet for decoding.
	int send_packet_result;
	{
		ZoneScopedN("avcodec_send_packet");
		send_packet_result = avcodec_send_packet(codec_context, p_packet);
	}
	// Note: EAGAIN can be returned if there's too many pending frames, which we have to read,
	// otherwise we would get stuck in an infinite loop.
	if (send_packet_result == 0 || send_packet_result == -EAGAIN) {
		_read_decoded_frames(p_receive_frame);
	} else {
		print_line(vformat("Failed to send avcodec packet: %s", ffmpeg_get_error_message(send_packet_result)));
		_try_disable_hw_decoding(send_packet_result);
	}

	return send_packet_result;
}

void VideoDecoder::_try_disable_hw_decoding(int p_error_code) {
	if (!hw_decoding_allowed || target_hw_video_decoders == HardwareVideoDecoder::NONE || codec_context == nullptr || codec_context->hw_device_ctx == nullptr) {
		return;
	}

	hw_decoding_allowed = false;

	if (p_error_code == -ENOMEM) {
		print_line("Disabling hardware decoding of video due to a lack of memory");
		target_hw_video_decoders = HardwareVideoDecoder::NONE;
	} else {
		print_line("Disabling hardware decoding of the video due to an unexpected error");
	}
	decoder_commands.push(this, &VideoDecoder::recreate_codec_context);
}

int created_texture = 0;

void VideoDecoder::_read_decoded_frames(AVFrame *p_received_frame) {
	Ref<Image> image;
	while (true) {
		ZoneScopedN("Video decoder read decoded frame");
		int receive_frame_result = avcodec_receive_frame(codec_context, p_received_frame);

		if (receive_frame_result < 0) {
			if (receive_frame_result != -EAGAIN && receive_frame_result != AVERROR_EOF) {
				print_line(vformat("Failed to receive frame from avcodec: %s", ffmpeg_get_error_message(receive_frame_result)));
				_try_disable_hw_decoding(receive_frame_result);
			}

			break;
		}

		// use `best_effort_timestamp` as it can be more accurate if timestamps from the source file (pts) are broken.
		// but some HW codecs don't set it in which case fallback to `pts`
		int64_t frame_timestamp = p_received_frame->best_effort_timestamp != AV_NOPTS_VALUE ? p_received_frame->best_effort_timestamp : p_received_frame->pts;
		double frame_time = (frame_timestamp - stream->start_time) * time_base_in_seconds * 1000.0;

		if (skip_output_until_time > frame_time) {
			continue;
		}

		Ref<FFmpegFrame> frame;
		if (is_hardware_pixel_format((AVPixelFormat)p_received_frame->format)) {
			Ref<FFmpegFrame> hw_transfer_frame;
			if (hw_transfer_frames.size() > 0) {
				hw_transfer_frame = hw_transfer_frames[0];
				hw_transfer_frames.pop_front();
			}

			if (!hw_transfer_frame.is_valid()) {
				hw_transfer_frame = Ref<FFmpegFrame>(memnew(FFmpegFrame(callable_mp(this, &VideoDecoder::_hw_transfer_frame_return))));
			}

			int transfer_result = av_hwframe_transfer_data(hw_transfer_frame->get_frame(), p_received_frame, 0);

			if (transfer_result < 0) {
				print_line("Failed to transfer frame from HW decoder:", ffmpeg_get_error_message(transfer_result));
				_try_disable_hw_decoding(transfer_result);
				continue;
			}

			frame = hw_transfer_frame;
		} else {
			// copy data to a new AVFrame so that `receiveFrame` can be reused.
			frame = Ref<FFmpegFrame>(memnew(FFmpegFrame(Callable())));
			av_frame_move_ref(frame->get_frame(), p_received_frame);
		}

		last_decoded_frame_time.set(frame_time);

		// Note: this is the pixel format that the video texture expects internally
		frame = _ensure_frame_pixel_format(frame, AVPixelFormat::AV_PIX_FMT_RGBA);
		if (!frame.is_valid()) {
			continue;
		}

		ZoneNamedN(image_unwrap, "Image unwrap", true);
		// Unwrap the image
		int width = frame->get_frame()->width;
		int height = frame->get_frame()->height;
		{
			ZoneNamedN(image_unwrap_copy, "Image unwrap copy", true);

			Vector<uint8_t> unwrapped_frame;
			int frame_size = frame->get_frame()->buf[0]->size; // Change this if we ever allow RGBA
			if (unwrapped_frame.size() != frame_size) {
				unwrapped_frame.resize(frame_size);
			}
			uint8_t *unwrapped_frame_ptrw = unwrapped_frame.ptrw();
			{
				ZoneNamedN(image_unwrap_memcopy, "memcpy", true);
				memcpy(unwrapped_frame_ptrw, frame->get_frame()->data[0], frame_size);
			}
			unwrapped_frame.resize(width * height * 4);
			if (!image.is_valid()) {
				image = Image::create_from_data(width, height, false, Image::FORMAT_RGBA8, unwrapped_frame);
			} else {
				image->set_data(width, height, false, Image::FORMAT_RGBA8, unwrapped_frame);
			}
		}
#ifdef FFMPEG_MT_GPU_UPLOAD
		Ref<ImageTexture> tex;
		available_textures_mutex.lock();
		if (available_textures.size() > 0) {
			tex = available_textures[0];
			available_textures.pop_front();
		}
		available_textures_mutex.unlock();
		{
			ZoneNamedN(image_unwrap_gpu, "Image unwrap GPU upload", true);
			if (!tex.is_valid() || tex->get_size() != image->get_size() || tex->get_format() != image->get_format()) {
				ZoneNamedN(image_unwrap_gpu_texture_create, "Image unwrap GPU texture create", true);
				tex = ImageTexture::create_from_image(image);
			} else {
				ZoneNamedN(image_unwrap_gpu_texture_update, "Image unwrap GPU texture update", true);
				tex->update(image);
			}
		}
		decoded_frames_mutex.lock();
		decoded_frames.push_back(memnew(DecodedFrame(frame_time, tex)));
		decoded_frames_mutex.unlock();
#else
		decoded_frames_mutex.lock();
		decoded_frames.push_back(memnew(DecodedFrame(frame_time, image)));
		decoded_frames_mutex.unlock();
#endif
	}
}

void VideoDecoder::_hw_transfer_frame_return(Ref<FFmpegFrame> p_hw_frame) {
	hw_transfer_frames.push_back(p_hw_frame);
}

void VideoDecoder::_scaler_frame_return(Ref<FFmpegFrame> p_scaler_frame) {
	scaler_frames.push_back(p_scaler_frame);
}

Ref<FFmpegFrame> VideoDecoder::_ensure_frame_pixel_format(Ref<FFmpegFrame> p_frame, AVPixelFormat p_target_pixel_format) {
	ZoneScopedN("Video decoder rescale");
	if (p_frame->get_frame()->format == p_target_pixel_format) {
		return p_frame;
	}

	int width = p_frame->get_frame()->width;
	int height = p_frame->get_frame()->height;

	if (p_frame->get_frame()->format == AV_PIX_FMT_NV12) {
		p_target_pixel_format = AV_PIX_FMT_YUYV422;
	}

	sws_context = sws_getCachedContext(
			sws_context,
			width, height, (AVPixelFormat)p_frame->get_frame()->format,
			width, height, p_target_pixel_format,
			1, nullptr, nullptr, nullptr);

	Ref<FFmpegFrame> scaler_frame;
	{
		if (scaler_frames.size() > 0) {
			scaler_frame = scaler_frames[0];
			scaler_frames.pop_front();
		}
	}

	if (!scaler_frame.is_valid()) {
		scaler_frame = Ref<FFmpegFrame>(memnew(FFmpegFrame(callable_mp(this, &VideoDecoder::_scaler_frame_return))));
	}

	// (re)initialize the scaler frame if needed.
	if (scaler_frame->get_frame()->format != p_target_pixel_format || scaler_frame->get_frame()->width != width || scaler_frame->get_frame()->height != height) {
		av_frame_unref(scaler_frame->get_frame());

		// Note: this field determines the scaler's output pix format.
		scaler_frame->get_frame()->format = p_target_pixel_format;
		scaler_frame->get_frame()->width = width;
		scaler_frame->get_frame()->height = height;

		int get_buffer_result = av_frame_get_buffer(scaler_frame->get_frame(), 0);

		if (get_buffer_result < 0) {
			print_line("Failed to allocate SWS frame buffer:", ffmpeg_get_error_message(get_buffer_result));
			p_frame->do_return();
			return Ref<FFmpegFrame>();
		}
	}

	int scaler_result = sws_scale(
			sws_context,
			p_frame->get_frame()->data, p_frame->get_frame()->linesize, 0, height,
			scaler_frame->get_frame()->data, scaler_frame->get_frame()->linesize);

	// return the original frame regardless of the scaler result.
	p_frame->do_return();

	if (scaler_result < 0) {
		print_line("Failed to scale frame:", ffmpeg_get_error_message(scaler_result));
		return Ref<FFmpegFrame>();
	}

	return scaler_frame;
}

void VideoDecoder::seek(double p_time) {
	decoder_commands.push(this, &VideoDecoder::_seek_command, p_time);
}

void VideoDecoder::start_decoding() {
	ERR_FAIL_COND_MSG(thread != nullptr, "Cannot start decoding once already started");
	if (format_context == nullptr) {
		prepare_decoding();
		recreate_codec_context();

		if (stream == nullptr) {
			decoder_state = DecoderState::FAULTED;
			return;
		}
	}

	thread = memnew(Thread);
	thread->start(&VideoDecoder::_thread_func, (void *)this);

	for (int i = 0; i < MAX_PENDING_FRAMES; i++) {
		work_semaphore.post();
	}
}

int get_hw_video_decoder_score(AVHWDeviceType p_device_type) {
	switch (p_device_type) {
		case AV_HWDEVICE_TYPE_VDPAU: {
			return 10;
		} break;
		case AV_HWDEVICE_TYPE_CUDA: {
			return 10;
		} break;
		case AV_HWDEVICE_TYPE_VAAPI: {
			return 9;
		} break;
		case AV_HWDEVICE_TYPE_DXVA2: {
			return 8;
		} break;
		case AV_HWDEVICE_TYPE_QSV: {
			return 9;
		} break;
		case AV_HWDEVICE_TYPE_MEDIACODEC: {
			return 10;
		} break;
		default: {
		} break;
	}
	return INT_MIN;
}

struct AvailableDecoderInfoComparator {
	bool operator()(const VideoDecoder::AvailableDecoderInfo &p_a, const VideoDecoder::AvailableDecoderInfo &p_b) const {
		return get_hw_video_decoder_score(p_a.device_type) > get_hw_video_decoder_score(p_b.device_type);
	}
};

Vector<VideoDecoder::AvailableDecoderInfo> VideoDecoder::get_available_decoders(const AVInputFormat *p_format, AVCodecID p_codec_id, BitField<HardwareVideoDecoder> p_target_decoders) {
	Vector<VideoDecoder::AvailableDecoderInfo> codecs;

	Ref<FFmpegCodec> first_codec;

	void *iterator = NULL;
	while (true) {
		const AVCodec *av_codec = av_codec_iterate(&iterator);

		if (av_codec == NULL) {
			break;
		}

		if (av_codec->id != p_codec_id || !av_codec_is_decoder(av_codec)) {
			continue;
		}

		Ref<FFmpegCodec> codec = memnew(FFmpegCodec(av_codec));
		if (!first_codec.is_valid()) {
			first_codec = codec;
		}

		if (p_target_decoders == HardwareVideoDecoder::NONE) {
			break;
		}

		for (AVHWDeviceType type : codec->get_supported_hw_device_types()) {
			HardwareVideoDecoder hw_video_decoder = from_av_hw_device_type(type);
			if (hw_video_decoder == NONE || !p_target_decoders.has_flag(hw_video_decoder)) {
				continue;
			}
			codecs.push_back(AvailableDecoderInfo{
					codec,
					type });
		}
	}

	// default to the first codec that we found with no HW devices.
	// The first codec is what FFmpeg's `avcodec_find_decoder` would return so this way we'll automatically fallback to that.
	if (first_codec.is_valid()) {
		codecs.push_back(AvailableDecoderInfo{
				first_codec,
				AV_HWDEVICE_TYPE_NONE });
	}

	codecs.sort_custom<AvailableDecoderInfoComparator>();
	return codecs;
}

void VideoDecoder::return_frames(Vector<Ref<DecodedFrame>> p_frames) {
	for (Ref<DecodedFrame> frame : p_frames) {
		return_frame(frame);
	}
}

void VideoDecoder::return_frame(Ref<DecodedFrame> p_frame) {
	MutexLock lock(available_textures_mutex);
	available_textures.push_back(p_frame->get_texture());
}

Vector<Ref<DecodedFrame>> VideoDecoder::get_decoded_frames() {
	Vector<Ref<DecodedFrame>> frames;
	MutexLock lock(decoded_frames_mutex);
	frames = decoded_frames.duplicate();
	decoded_frames.clear();
	for (int i = 0; i < decoded_frames.size(); i++) {
		work_semaphore.post();
	}
	return frames;
}

VideoDecoder::DecoderState VideoDecoder::get_decoder_state() const {
	return decoder_state;
}

double VideoDecoder::get_last_decoded_frame_time() const {
	return last_decoded_frame_time.get();
}

bool VideoDecoder::is_running() const {
	return decoder_state == DecoderState::RUNNING;
}

double VideoDecoder::get_duration() const {
	return duration;
}

VideoDecoder::VideoDecoder(Ref<FileAccess> p_file) :
		decoder_commands(true) {
	video_stream = p_file;
}

VideoDecoder::~VideoDecoder() {
	if (thread != nullptr) {
		thread_abort.set_to(true);
		thread->wait_to_finish();
	}

	if (format_context != nullptr && input_opened) {
		avformat_close_input(&format_context);
	}

	if (codec_context != nullptr) {
		avcodec_free_context(&codec_context);
	}

	if (sws_context != nullptr) {
		sws_freeContext(sws_context);
	}
}

DecodedFrame::DecodedFrame(double p_time, Ref<ImageTexture> p_texture) {
	time = p_time;
	texture = p_texture;
}

DecodedFrame::DecodedFrame(double p_time, Ref<Image> p_image) {
	time = p_time;
	image = p_image;
}

Ref<ImageTexture> DecodedFrame::get_texture() const { return texture; }

void DecodedFrame::set_texture(const Ref<ImageTexture> &p_texture) { texture = p_texture; }

double DecodedFrame::get_time() const { return time; }

void DecodedFrame::set_time(double p_time) { time = p_time; }
