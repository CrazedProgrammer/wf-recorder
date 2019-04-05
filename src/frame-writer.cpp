// Adapted from https://stackoverflow.com/questions/34511312/how-to-encode-a-video-from-several-images-generated-in-a-c-program-without-wri
// (Later) adapted from https://github.com/apc-llc/moviemaker-cpp

#include <iostream>
#include "frame-writer.hpp"
#include <vector>
#include <cstring>
#include "averr.h"

#define FPS 60

class FFmpegInitialize
{
public :

    FFmpegInitialize()
    {
        // Loads the whole database of available codecs and formats.
        av_register_all();
    }
};

static FFmpegInitialize ffmpegInitialize;

void FrameWriter::init_hw_accel()
{
    int ret = av_hwdevice_ctx_create(&this->hw_device_context,
        av_hwdevice_find_type_by_name("vaapi"), params.hw_device.c_str(), NULL, 0);

    if (ret != 0)
    {
        std::cerr << "Failed to create hw encoding device " << params.hw_device << ": " << averr(ret) << std::endl;
        std::exit(-1);
    }

    this->hw_frame_context = av_hwframe_ctx_alloc(hw_device_context);
    if (!this->hw_frame_context)
    {
        std::cerr << "Failed to initialize hw frame context" << std::endl;
        av_buffer_unref(&hw_device_context);
        std::exit(-1);
    }

    AVHWFramesConstraints *cst;
    cst = av_hwdevice_get_hwframe_constraints(hw_device_context, NULL);
    if (!cst)
    {
        std::cerr << "Failed to get hwframe constraints" << std::endl;
        av_buffer_unref(&hw_device_context);
        std::exit(-1);
    }

    AVHWFramesContext *ctx = (AVHWFramesContext*)this->hw_frame_context->data;
    ctx->width = params.width;
    ctx->height = params.height;
    ctx->format = cst->valid_hw_formats[0];
    ctx->sw_format = AV_PIX_FMT_NV12;

    if ((ret = av_hwframe_ctx_init(hw_frame_context)))
    {
        std::cerr << "Failed to initialize hwframe context: " << averr(ret) << std::endl;
        av_buffer_unref(&hw_device_context);
        av_buffer_unref(&hw_frame_context);
        std::exit(-1);
    }
}


void FrameWriter::load_codec_options(AVDictionary **dict)
{
    static const std::map<std::string, std::string> default_x264_options = {
        {"tune", "zerolatency"},
        {"preset", "ultrafast"},
        {"crf", "20"},
    };

    if (params.codec.find("libx264") != std::string::npos ||
        params.codec.find("libx265") != std::string::npos)
    {
        for (const auto& param : default_x264_options)
        {
            if (!params.codec_options.count(param.first))
                params.codec_options[param.first] = param.second;
        }
    }

    for (auto& opt : params.codec_options)
    {
        std::cout << "Setting codec option: " << opt.first << "=" << opt.second << std::endl;
        av_dict_set(dict, opt.first.c_str(), opt.second.c_str(), 0);
    }
}

bool is_fmt_supported(AVPixelFormat fmt, const AVPixelFormat *supported)
{
    for (int i = 0; supported[i] != AV_PIX_FMT_NONE; i++)
    {
        if (supported[i] == fmt)
            return true;
    }

    return false;
}

AVPixelFormat FrameWriter::get_input_format()
{
    return params.format == INPUT_FORMAT_BGR0 ?
        AV_PIX_FMT_BGR0 : AV_PIX_FMT_RGB0;
}

AVPixelFormat FrameWriter::choose_sw_format(AVCodec *codec)
{
    /* First case: if the codec supports getting the appropriate RGB format
     * directly, we want to use it since we don't have to convert data */
    auto in_fmt = get_input_format();
    if (is_fmt_supported(in_fmt, codec->pix_fmts))
        return in_fmt;

    /* Otherwise, try to use the already tested YUV420p */
    if (is_fmt_supported(AV_PIX_FMT_YUV420P, codec->pix_fmts))
        return AV_PIX_FMT_YUV420P;

    /* Lastly, use the first supported format */
    return codec->pix_fmts[0];
}

void FrameWriter::init_codec()
{
    AVDictionary *options = NULL;
    load_codec_options(&options);

    AVCodec* codec = avcodec_find_encoder_by_name(params.codec.c_str());
    if (!codec)
    {
        std::cerr << "Failed to find the given codec" << std::endl;
        std::exit(-1);
    }

    stream = avformat_new_stream(fmtCtx, codec);
    if (!stream)
    {
        std::cerr << "Failed to open stream" << std::endl;
        std::exit(-1);
    }

    codecCtx = stream->codec;
    codecCtx->width = params.width;
    codecCtx->height = params.height;
    codecCtx->time_base = (AVRational){ 1, FPS };

    if (params.codec.find("vaapi") != std::string::npos)
    {
        codecCtx->pix_fmt = AV_PIX_FMT_VAAPI;
        init_hw_accel();
        codecCtx->hw_frames_ctx = av_buffer_ref(hw_frame_context);
    } else
    {
        codecCtx->pix_fmt = choose_sw_format(codec);
        std::cout << "Choosing pixel format " <<
            av_get_pix_fmt_name(codecCtx->pix_fmt) << std::endl;
        init_sws();
    }

    if (fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
        codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int err;
    if ((err = avcodec_open2(codecCtx, codec, &options)) < 0)
    {
        std::cerr << "avcodec_open2 failed: " << averr(err) << std::endl;
        std::exit(-1);
    }
    av_dict_free(&options);

    // Once the codec is set up, we need to let the container know
    // which codec are the streams using, in this case the only (video) stream.
    stream->time_base = (AVRational){ 1, FPS };
    av_dump_format(fmtCtx, 0, params.file.c_str(), 1);
    if (avio_open(&fmtCtx->pb, params.file.c_str(), AVIO_FLAG_WRITE))
    {
        std::cerr << "avio_open failed" << std::endl;
        std::exit(-1);
    }

    AVDictionary *dummy = NULL;
    if (avformat_write_header(fmtCtx, &dummy) != 0)
    {
        std::cerr << "Failed to write file header" << std::endl;
        std::exit(-1);
    }
    av_dict_free(&dummy);
}

void FrameWriter::init_sws()
{
    swsCtx = sws_getContext(params.width, params.height, get_input_format(),
        params.width, params.height, codecCtx->pix_fmt, SWS_FAST_BILINEAR, NULL, NULL, NULL);

    if (!swsCtx)
    {
        std::cerr << "Failed to create sws context" << std::endl;
        std::exit(-1);
    }
}

FrameWriter::FrameWriter(const FrameWriterParams& _params) :
    params(_params)
{
    if (params.enable_ffmpeg_debug_output)
        av_log_set_level(AV_LOG_DEBUG);

    // Preparing the data concerning the format and codec,
    // in order to write properly the header, frame data and end of file.
    this->outputFmt = av_guess_format(NULL, params.file.c_str(), NULL);
    if (!outputFmt)
    {
        std::cerr << "Failed to guess output format for file " << params.file << std::endl;
        std::exit(-1);
    }

    if (avformat_alloc_output_context2(&this->fmtCtx, NULL, NULL, params.file.c_str()) < 0)
    {
        std::cerr << "Failed to allocate output context" << std::endl;
        std::exit(-1);
    }

    init_codec();

    encoder_frame = av_frame_alloc();
    if (hw_device_context) {
        encoder_frame->format = get_input_format();
    } else {
        encoder_frame->format = codecCtx->pix_fmt;
    }
    encoder_frame->width = params.width;
    encoder_frame->height = params.height;
    if (av_frame_get_buffer(encoder_frame, 1))
    {
        std::cerr << "Failed to allocate frame buffer" << std::endl;
        std::exit(-1);
    }

    if (hw_device_context)
    {
        hw_frame = av_frame_alloc();
        AVHWFramesContext *frctx = (AVHWFramesContext*)hw_frame_context->data;
        hw_frame->format = frctx->format;
        hw_frame->hw_frames_ctx = av_buffer_ref(hw_frame_context);
        hw_frame->width = params.width;
        hw_frame->height = params.height;

        if (av_hwframe_get_buffer(hw_frame_context, hw_frame, 0))
        {
            std::cerr << "failed to hw frame buffer" << std::endl;
            std::exit(-1);
        }
    }
}

void FrameWriter::add_frame(const uint8_t* pixels, int msec, bool y_invert)
{
    /* Calculate data after y-inversion */
    int stride[] = {int(4 * params.width)};
    const uint8_t *formatted_pixels = pixels;
    if (y_invert)
    {
        formatted_pixels += stride[0] * (params.height - 1);
        stride[0] *= -1;
    }

    AVFrame **output_frame;
    if (hw_device_context)
    {
        encoder_frame->data[0] = (uint8_t*)formatted_pixels;
        encoder_frame->linesize[0] = stride[0];

        if (av_hwframe_transfer_data(hw_frame, encoder_frame, 0))
        {
            std::cerr << "Failed to upload data to the gpu!" << std::endl;
            return;
        }

        output_frame = &hw_frame;
    } else if(get_input_format() == codecCtx->pix_fmt)
    {
        output_frame = &encoder_frame;
        encoder_frame->data[0] = (uint8_t*)formatted_pixels;
        encoder_frame->linesize[0] = stride[0];
    } else
    {
        sws_scale(swsCtx, &formatted_pixels, stride, 0, params.height,
            encoder_frame->data, encoder_frame->linesize);

        output_frame = &encoder_frame;
    }

    (*output_frame)->pts = msec;

    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    int got_output;
    avcodec_encode_video2(codecCtx, &pkt, *output_frame, &got_output);
    if (got_output)
      finish_frame();
}

void FrameWriter::finish_frame()
{
    static int iframe = 0;
    av_packet_rescale_ts(&pkt, (AVRational){ 1, 1000 }, stream->time_base);

    pkt.stream_index = stream->index;

    av_interleaved_write_frame(fmtCtx, &pkt);
    av_packet_unref(&pkt);

    printf("Wrote frame %d\n", iframe++);
    fflush(stdout);
}

FrameWriter::~FrameWriter()
{
    // Writing the delayed frames:
    for (int got_output = 1; got_output;)
    {
        avcodec_encode_video2(codecCtx, &pkt, NULL, &got_output);
        if (got_output)
            finish_frame();
    }

    // Writing the end of the file.
    av_write_trailer(fmtCtx);

    // Closing the file.
    if (!(outputFmt->flags & AVFMT_NOFILE))
        avio_closep(&fmtCtx->pb);
    avcodec_close(stream->codec);

    // Freeing all the allocated memory:
    sws_freeContext(swsCtx);
    av_frame_free(&encoder_frame);
    // TODO: free all the hw accel
    avformat_free_context(fmtCtx);
}
