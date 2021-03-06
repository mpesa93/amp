////////////////////////////////////////////////////////////////////////////////
//
// plugins/flac/input.cpp
//
////////////////////////////////////////////////////////////////////////////////


#include <amp/audio/codec.hpp>
#include <amp/audio/format.hpp>
#include <amp/audio/input.hpp>
#include <amp/audio/packet.hpp>
#include <amp/audio/pcm.hpp>
#include <amp/error.hpp>
#include <amp/io/memory.hpp>
#include <amp/io/stream.hpp>
#include <amp/media/image.hpp>
#include <amp/media/tags.hpp>
#include <amp/numeric.hpp>
#include <amp/range.hpp>
#include <amp/stddef.hpp>
#include <amp/u8string.hpp>
#include <amp/utility.hpp>

#include <cstddef>
#include <cstdio>
#include <iterator>
#include <memory>
#include <string_view>
#include <utility>

#include <FLAC/callback.h>
#include <FLAC/format.h>
#include <FLAC/metadata.h>
#include <FLAC/stream_decoder.h>


namespace std {

template<>
struct default_delete<FLAC__StreamDecoder>
{
    void operator()(FLAC__StreamDecoder* const decoder) const noexcept
    { FLAC__stream_decoder_delete(decoder); }
};

template<>
struct default_delete<FLAC__Metadata_Iterator>
{
    void operator()(FLAC__Metadata_Iterator* const iter) const noexcept
    { FLAC__metadata_iterator_delete(iter); }
};

template<>
struct default_delete<FLAC__Metadata_Chain>
{
    void operator()(FLAC__Metadata_Chain* const chain) const noexcept
    { FLAC__metadata_chain_delete(chain); }
};

}     // namespace std


namespace amp {
namespace flac {
namespace {

class metadata_chain
{
public:
    class iterator
    {
    public:
        using value_type        = FLAC__StreamMetadata;
        using pointer           = FLAC__StreamMetadata*;
        using reference         = FLAC__StreamMetadata&;
        using difference_type   = std::ptrdiff_t;
        using iterator_category = std::input_iterator_tag;

        iterator() noexcept :
            iter_{}
        {}

        iterator& operator++()
        {
            if (!FLAC__metadata_iterator_next(iter_)) {
                iter_ = nullptr;
            }
            return *this;
        }

        pointer operator->() const
        { return FLAC__metadata_iterator_get_block(iter_); }

        reference operator*() const
        { return *operator->(); }

        bool operator==(iterator const& x) const noexcept
        { return (iter_ == x.iter_); }

        bool operator!=(iterator const& x) const noexcept
        { return !(*this == x); }

    private:
        friend class metadata_chain;

        explicit iterator(FLAC__Metadata_Iterator* const x) noexcept :
            iter_{x}
        {}

        FLAC__Metadata_Iterator* iter_;
    };

    iterator begin() const
    { return iterator{first_.get()}; }

    iterator end() const noexcept
    { return iterator{};  }

    explicit metadata_chain(io::stream& file, bool const is_ogg) :
        chain_{FLAC__metadata_chain_new()},
        first_{FLAC__metadata_iterator_new()}
    {
        if (AMP_UNLIKELY(!chain_ || !first_)) {
            raise_bad_alloc();
        }

        static constexpr FLAC__IOCallbacks callbacks {
            metadata_chain::read,
            metadata_chain::write,
            metadata_chain::seek,
            metadata_chain::tell,
            metadata_chain::eof,
            nullptr,
        };

        auto const read = is_ogg
                        ? &FLAC__metadata_chain_read_ogg_with_callbacks
                        : &FLAC__metadata_chain_read_with_callbacks;
        if (AMP_UNLIKELY(!(*read)(chain_.get(), &file, callbacks))) {
            raise(errc::failure, "failed to read FLAC stream metadata");
        }

        //FLAC__metadata_chain_merge_padding(chain_.get());
        FLAC__metadata_iterator_init(first_.get(), chain_.get());
    }

private:
    static remove_pointer_t<FLAC__IOCallback_Read> read;
    static remove_pointer_t<FLAC__IOCallback_Write> write;
    static remove_pointer_t<FLAC__IOCallback_Seek> seek;
    static remove_pointer_t<FLAC__IOCallback_Tell> tell;
    static remove_pointer_t<FLAC__IOCallback_Eof> eof;

    std::unique_ptr<FLAC__Metadata_Chain> chain_;
    std::unique_ptr<FLAC__Metadata_Iterator> first_;
};


std::size_t metadata_chain::read(void* const dst, std::size_t const size,
                                 std::size_t const n, void* const opaque)
{
    try {
        auto&& file = *static_cast<io::stream*>(opaque);
        return file.try_read(static_cast<uchar*>(dst), size * n);
    }
    catch (...) {
        return -1_sz;
    }
}

std::size_t metadata_chain::write(void const* const src, std::size_t const size,
                                  std::size_t const n, void* const opaque)
{
    try {
        auto&& file = *static_cast<io::stream*>(opaque);
        auto const bytes = size * n;
        file.write(src, bytes);
        return bytes;
    }
    catch (...) {
        return -1_sz;
    }
}

int metadata_chain::seek(void* const opaque, int64 const off, int const whence)
{
    try {
        auto&& file = *static_cast<io::stream*>(opaque);
        file.seek(off, static_cast<io::seekdir>(whence));
        return 0;
    }
    catch (...) {
        return -1;
    }
}

int64 metadata_chain::tell(void* const opaque)
{
    try {
        auto&& file = *static_cast<io::stream*>(opaque);
        return static_cast<int64>(file.tell());
    }
    catch (...) {
        return -1;
    }
}

int metadata_chain::eof(void* const opaque)
{
    auto&& file = *static_cast<io::stream*>(opaque);
    return file.eof();
}


[[noreturn]] void raise(FLAC__StreamDecoderState const state)
{
    errc ec;
    switch (state) {
    case FLAC__STREAM_DECODER_MEMORY_ALLOCATION_ERROR:
        raise_bad_alloc();
    case FLAC__STREAM_DECODER_END_OF_STREAM:
        ec = errc::end_of_file;
        break;
    case FLAC__STREAM_DECODER_SEEK_ERROR:
        ec = errc::seek_error;
        break;
    case FLAC__STREAM_DECODER_OGG_ERROR:
    case FLAC__STREAM_DECODER_ABORTED:
        ec = errc::read_fault;
        break;
    case FLAC__STREAM_DECODER_SEARCH_FOR_METADATA:
    case FLAC__STREAM_DECODER_READ_METADATA:
    case FLAC__STREAM_DECODER_SEARCH_FOR_FRAME_SYNC:
    case FLAC__STREAM_DECODER_READ_FRAME:
    case FLAC__STREAM_DECODER_UNINITIALIZED:
        ec = errc::failure;
        break;
    }
    raise(ec, "FLAC: %s", FLAC__StreamDecoderStateString[state]);
}

[[noreturn]] void raise(FLAC__StreamDecoderInitStatus const status)
{
    errc ec;
    switch (status) {
    case FLAC__STREAM_DECODER_INIT_STATUS_MEMORY_ALLOCATION_ERROR:
        raise_bad_alloc();
    case FLAC__STREAM_DECODER_INIT_STATUS_UNSUPPORTED_CONTAINER:
        ec = errc::protocol_not_supported;
        break;
    case FLAC__STREAM_DECODER_INIT_STATUS_OK:
    case FLAC__STREAM_DECODER_INIT_STATUS_INVALID_CALLBACKS:
    case FLAC__STREAM_DECODER_INIT_STATUS_ERROR_OPENING_FILE:
    case FLAC__STREAM_DECODER_INIT_STATUS_ALREADY_INITIALIZED:
        ec = errc::invalid_data_format;
        break;
    }
    raise(ec, "FLAC: %s", FLAC__StreamDecoderInitStatusString[status]);
}


inline bool is_ogg_stream(io::stream& file)
{
    id3v2::skip(file);

    uint8 buf[33];
    file.peek(buf, sizeof(buf));

    if (io::load<uint32,BE>(buf) == "fLaC"_4cc) {
        return false;
    }
    if (io::load<uint32,BE>(buf +  0) == "OggS"_4cc &&
        io::load<uint32,BE>(buf + 29) == "FLAC"_4cc) {
        return true;
    }

    raise(errc::invalid_data_format, "no FLAC file signature");
}


class input
{
public:
    explicit input(ref_ptr<io::stream>, audio::open_mode);

    void read(audio::packet&);
    void seek(uint64);

    auto get_format() const noexcept;
    auto get_info(uint32);
    auto get_image(media::image::type);
    auto get_chapter_count() const noexcept;

private:
    static remove_pointer_t<FLAC__StreamDecoderReadCallback>     read;
    static remove_pointer_t<FLAC__StreamDecoderSeekCallback>     seek;
    static remove_pointer_t<FLAC__StreamDecoderTellCallback>     tell;
    static remove_pointer_t<FLAC__StreamDecoderLengthCallback>   length;
    static remove_pointer_t<FLAC__StreamDecoderEofCallback>      eof;
    static remove_pointer_t<FLAC__StreamDecoderWriteCallback>    write;
    static remove_pointer_t<FLAC__StreamDecoderMetadataCallback> metadata;
    static remove_pointer_t<FLAC__StreamDecoderErrorCallback>    error;

    ref_ptr<io::stream> const file_;
    audio::packet readbuf_;
    std::unique_ptr<audio::pcm::blitter> blitter_;
    std::unique_ptr<FLAC__StreamDecoder> decoder_;
    FLAC__StreamMetadata_StreamInfo info_;
    uint64 last_pos_{};
    uint32 avg_bit_rate_;
    bool const is_ogg_;
};

FLAC__StreamDecoderReadStatus
input::read(FLAC__StreamDecoder const*, uint8* const dst,
            std::size_t* const size, void* const opaque)
{
    try {
        *size = static_cast<input*>(opaque)->file_->try_read(dst, *size);
        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }
    catch (...) {
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }
}

FLAC__StreamDecoderSeekStatus
input::seek(FLAC__StreamDecoder const*, uint64 const pos, void* const opaque)
{
    try {
        static_cast<input*>(opaque)->file_->seek(pos);
        return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
    }
    catch (...) {
        return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }
}

FLAC__StreamDecoderTellStatus
input::tell(FLAC__StreamDecoder const*, uint64* const pos, void* const opaque)
{
    try {
        *pos = static_cast<input*>(opaque)->file_->tell();
        return FLAC__STREAM_DECODER_TELL_STATUS_OK;
    }
    catch (...) {
        return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
    }
}

FLAC__StreamDecoderLengthStatus
input::length(FLAC__StreamDecoder const*, uint64* const len,
              void* const opaque)
{
    try {
        *len = static_cast<input*>(opaque)->file_->size();
        return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
    }
    catch (...) {
        return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
    }
}

int input::eof(FLAC__StreamDecoder const*, void* const opaque)
{
    return static_cast<input*>(opaque)->file_->eof();
}

FLAC__StreamDecoderWriteStatus
input::write(FLAC__StreamDecoder const*, FLAC__Frame const* const frame,
             int32 const* const* const source, void* const opaque)
{
    try {
        auto&& self = *static_cast<input*>(opaque);
        self.blitter_->convert(source, frame->header.blocksize, self.readbuf_);
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }
    catch (...) {
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
}

void input::metadata(FLAC__StreamDecoder const*,
                     FLAC__StreamMetadata const* const metadata,
                     void* const opaque)
{
    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
        static_cast<input*>(opaque)->info_ = metadata->data.stream_info;
    }
}

void input::error(FLAC__StreamDecoder const*,
                  FLAC__StreamDecoderErrorStatus const status, void*)
{
    std::fprintf(stderr, "[FLAC] stream decoder error: %s\n",
                 FLAC__StreamDecoderErrorStatusString[status]);
}


input::input(ref_ptr<io::stream> s, audio::open_mode const mode) :
    file_(std::move(s)),
    is_ogg_(flac::is_ogg_stream(*file_))
{
    if (!(mode & (audio::playback | audio::metadata))) {
        return;
    }

    decoder_.reset(FLAC__stream_decoder_new());
    if (AMP_UNLIKELY(decoder_ == nullptr)) {
        raise_bad_alloc();
    }

    auto const init = is_ogg_ ? &FLAC__stream_decoder_init_ogg_stream
                              : &FLAC__stream_decoder_init_stream;
    auto const status = (*init)(decoder_.get(),
                                input::read,
                                input::seek,
                                input::tell,
                                input::length,
                                input::eof,
                                input::write,
                                input::metadata,
                                input::error,
                                this);
    if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        flac::raise(status);
    }
    if (!FLAC__stream_decoder_process_until_end_of_metadata(decoder_.get())) {
        flac::raise(FLAC__stream_decoder_get_state(decoder_.get()));
    }

    avg_bit_rate_ = static_cast<uint32>(muldiv(file_->size() - file_->tell(),
                                               info_.sample_rate * 8,
                                               info_.total_samples));

    // Avoid creating a PCM blitter if we're just reading metadata.
    if (mode & audio::playback) {
        audio::pcm::spec spec;
        spec.bytes_per_sample = 4;
        spec.bits_per_sample = info_.bits_per_sample;
        spec.channels = info_.channels;
        spec.flags = audio::pcm::signed_int
                   | audio::pcm::host_endian
                   | audio::pcm::non_interleaved;

        blitter_ = audio::pcm::blitter::create(spec);
        FLAC__stream_decoder_get_decode_position(decoder_.get(), &last_pos_);
    }
}

void input::read(audio::packet& pkt)
{
    if (AMP_LIKELY(readbuf_.empty())) {
        auto const ok = FLAC__stream_decoder_process_single(decoder_.get());
        if (AMP_UNLIKELY(!ok)) {
            flac::raise(FLAC__stream_decoder_get_state(decoder_.get()));
        }
    }
    readbuf_.swap(pkt);
    readbuf_.clear();

    auto bit_rate = avg_bit_rate_;
    if (pkt.frames() != 0) {
        uint64 pos;
        if (FLAC__stream_decoder_get_decode_position(decoder_.get(), &pos)) {
            if (pos > last_pos_) {
                bit_rate = static_cast<uint32>(muldiv(pos - last_pos_,
                                                      info_.sample_rate * 8,
                                                      pkt.frames()));
                last_pos_ = pos;
            }
        }
    }
    pkt.set_bit_rate(bit_rate);
}

void input::seek(uint64 const pts)
{
    readbuf_.clear();
    if (!FLAC__stream_decoder_seek_absolute(decoder_.get(), pts)) {
        readbuf_.clear();
        if (!FLAC__stream_decoder_flush(decoder_.get())) {
            flac::raise(FLAC__stream_decoder_get_state(decoder_.get()));
        }
    }
    FLAC__stream_decoder_get_decode_position(decoder_.get(), &last_pos_);
}

auto input::get_format() const noexcept
{
    audio::format ret;
    ret.sample_rate = info_.sample_rate;
    ret.channels = info_.channels;
    ret.channel_layout = audio::xiph_channel_layout(info_.channels);
    return ret;
}

auto input::get_info(uint32 const /* chapter_number */)
{
    audio::stream_info out{get_format()};
    out.codec_id         = audio::codec::flac;
    out.frames           = info_.total_samples;
    out.bits_per_sample  = info_.bits_per_sample;
    out.average_bit_rate = avg_bit_rate_;
    if (is_ogg_) {
        out.props.emplace(tags::container, "Ogg FLAC");
    }

    auto get_comment = [](auto const& comment) noexcept {
        return std::string_view {
            reinterpret_cast<char const*>(comment.entry),
            static_cast<std::size_t>(comment.length),
        };
    };

    for (auto&& block : flac::metadata_chain{*file_, is_ogg_}) {
        if (block.type != FLAC__METADATA_TYPE_VORBIS_COMMENT) {
            continue;
        }

        auto&& vc = block.data.vorbis_comment;
        out.props.try_emplace(tags::encoder, get_comment(vc.vendor_string));

        out.tags.reserve(out.tags.size() + vc.num_comments);
        for (auto const i : xrange(vc.num_comments)) {
            auto comment = get_comment(vc.comments[i]);
            if (!comment.empty()) {
                auto found = comment.find('=');
                if (found < (comment.size() - 1)) {
                    auto const key   = comment.substr(0, found);
                    auto const value = comment.substr(found + 1);
                    out.tags.emplace(tags::map_common_key(key), value);
                }
            }
        }
    }
    return out;
}

auto input::get_image(media::image::type const type)
{
    media::image image;
    for (auto&& block : flac::metadata_chain{*file_, is_ogg_}) {
        if (block.type != FLAC__METADATA_TYPE_PICTURE) {
            continue;
        }
        if (block.data.picture.type != as_underlying(type)) {
            continue;
        }

        auto&& pic = block.data.picture;
        image.set_data(pic.data, pic.data_length);
        image.set_mime_type(pic.mime_type);
        image.set_description(reinterpret_cast<char const*>(pic.description));
        break;
    }
    return image;
}

auto input::get_chapter_count() const noexcept
{
    return uint32{0};
}

AMP_REGISTER_INPUT(input, "fla", "flac", "oga", "ogg");

}}}   // namespace amp::flac::<unnamed>

