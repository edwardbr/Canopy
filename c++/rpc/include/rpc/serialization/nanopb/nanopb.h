/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <pb_decode.h>
#include <pb_encode.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace rpc::serialization::nanopb
{
    struct string_encode_state
    {
        const std::string* value{};
    };

    struct string_decode_state
    {
        std::string* value{};
    };

    struct bytes_encode_state
    {
        const void* data{};
        size_t size{};
    };

    struct bytes_decode_state
    {
        std::vector<uint8_t>* value{};
    };

    template<class T> struct repeated_varint_encode_state
    {
        const std::vector<T>* value{};
    };

    template<class T> struct repeated_varint_decode_state
    {
        std::vector<T>* value{};
    };

    template<class T> struct sequence_structure_encode_state
    {
        const std::vector<T>* value{};
    };

    template<class T> struct sequence_submessage_encode_state
    {
        const std::vector<T>* value{};
        const pb_msgdesc_t* fields{};
        bool (*encode)(
            pb_ostream_t*,
            const pb_msgdesc_t*,
            const T&){};
    };

    template<class T> struct sequence_structure_decode_state
    {
        std::vector<T>* value{};
    };

    template<class Map> struct dictionary_structure_encode_state
    {
        const Map* value{};
    };

    template<class Map> struct dictionary_submessage_encode_state
    {
        const Map* value{};
        const pb_msgdesc_t* value_fields{};
        bool (*encode_value)(
            pb_ostream_t*,
            const pb_msgdesc_t*,
            const typename std::remove_reference_t<Map>::mapped_type&){};
    };

    template<class Map> struct dictionary_structure_decode_state
    {
        Map* value{};
    };

    inline bool encode_string(
        pb_ostream_t* stream,
        const pb_field_t* field,
        void* const* arg)
    {
        auto* state = static_cast<const string_encode_state*>(*arg);
        if (!state || !state->value)
            return false;

        if (!pb_encode_tag_for_field(stream, field))
            return false;

        const auto& value = *state->value;
        return pb_encode_string(stream, reinterpret_cast<const pb_byte_t*>(value.data()), value.size());
    }

    inline bool decode_string(
        pb_istream_t* stream,
        const pb_field_t* field,
        void** arg)
    {
        (void)field;
        auto* state = static_cast<string_decode_state*>(*arg);
        if (!state || !state->value)
            return false;

        const auto old_size = state->value->size();
        state->value->resize(old_size + stream->bytes_left);
        if (stream->bytes_left == 0)
            return true;

        return pb_read(stream, reinterpret_cast<pb_byte_t*>(&(*state->value)[old_size]), state->value->size() - old_size);
    }

    inline bool encode_bytes(
        pb_ostream_t* stream,
        const pb_field_t* field,
        void* const* arg)
    {
        auto* state = static_cast<const bytes_encode_state*>(*arg);
        if (!state)
            return false;

        if (!pb_encode_tag_for_field(stream, field))
            return false;

        return pb_encode_string(stream, static_cast<const pb_byte_t*>(state->data), state->size);
    }

    inline bool decode_bytes(
        pb_istream_t* stream,
        const pb_field_t* field,
        void** arg)
    {
        (void)field;
        auto* state = static_cast<bytes_decode_state*>(*arg);
        if (!state || !state->value)
            return false;

        const auto old_size = state->value->size();
        state->value->resize(old_size + stream->bytes_left);
        if (stream->bytes_left == 0)
            return true;

        return pb_read(stream, reinterpret_cast<pb_byte_t*>(&(*state->value)[old_size]), state->value->size() - old_size);
    }

    template<class T>
    bool encode_repeated_varint(
        pb_ostream_t* stream,
        const pb_field_t* field,
        void* const* arg)
    {
        auto* state = static_cast<const repeated_varint_encode_state<T>*>(*arg);
        if (!state || !state->value)
            return false;

        for (const auto& value : *state->value)
        {
            if (!pb_encode_tag_for_field(stream, field))
                return false;
            if (!pb_encode_varint(stream, static_cast<uint64_t>(value)))
                return false;
        }

        return true;
    }

    template<class T>
    bool decode_repeated_varint(
        pb_istream_t* stream,
        const pb_field_t* field,
        void** arg)
    {
        (void)field;
        auto* state = static_cast<repeated_varint_decode_state<T>*>(*arg);
        if (!state || !state->value)
            return false;

        uint64_t value = 0;
        if (!pb_decode_varint(stream, &value))
            return false;

        state->value->push_back(static_cast<T>(value));
        return true;
    }

    inline bool encode_length_delimited_field(
        pb_ostream_t* stream,
        uint32_t tag,
        const void* data,
        size_t size)
    {
        if (!pb_encode_tag(stream, PB_WT_STRING, tag))
            return false;
        return pb_encode_string(stream, static_cast<const pb_byte_t*>(data), size);
    }

    inline bool read_length_delimited_bytes(
        pb_istream_t* stream,
        std::vector<char>& value)
    {
        pb_istream_t substream;
        if (!pb_make_string_substream(stream, &substream))
            return false;

        value.resize(substream.bytes_left);
        const bool ok = value.empty() || pb_read(&substream, reinterpret_cast<pb_byte_t*>(value.data()), value.size());
        return ok && pb_close_string_substream(stream, &substream);
    }

    inline bool read_length_delimited_string(
        pb_istream_t* stream,
        std::string& value)
    {
        pb_istream_t substream;
        if (!pb_make_string_substream(stream, &substream))
            return false;

        value.resize(substream.bytes_left);
        const bool ok = value.empty() || pb_read(&substream, reinterpret_cast<pb_byte_t*>(value.data()), value.size());
        return ok && pb_close_string_substream(stream, &substream);
    }

    template<class T>
    bool encode_sequence_structure(
        pb_ostream_t* stream,
        const pb_field_t* field,
        void* const* arg)
    {
        auto* state = static_cast<const sequence_structure_encode_state<T>*>(*arg);
        if (!state || !state->value)
            return false;

        for (const auto& value : *state->value)
        {
            std::vector<char> value_buffer;
            value.nanopb_serialise(value_buffer);
            if (!pb_encode_tag_for_field(stream, field))
                return false;
            if (!pb_encode_string(stream, reinterpret_cast<const pb_byte_t*>(value_buffer.data()), value_buffer.size()))
                return false;
        }

        return true;
    }

    template<class T>
    bool encode_sequence_submessage(
        pb_ostream_t* stream,
        const pb_field_t* field,
        void* const* arg)
    {
        auto* state = static_cast<const sequence_submessage_encode_state<T>*>(*arg);
        if (!state || !state->value || !state->fields || !state->encode)
            return false;

        for (const auto& value : *state->value)
        {
            if (!pb_encode_tag_for_field(stream, field))
                return false;
            if (!state->encode(stream, state->fields, value))
                return false;
        }

        return true;
    }

    template<class T>
    bool decode_sequence_structure(
        pb_istream_t* stream,
        const pb_field_t* field,
        void** arg)
    {
        (void)field;
        auto* state = static_cast<sequence_structure_decode_state<T>*>(*arg);
        if (!state || !state->value)
            return false;

        std::vector<char> value_buffer(stream->bytes_left);
        if (!value_buffer.empty()
            && !pb_read(stream, reinterpret_cast<pb_byte_t*>(value_buffer.data()), value_buffer.size()))
            return false;

        T value;
        value.nanopb_deserialise(value_buffer);
        state->value->push_back(std::move(value));
        return true;
    }

    template<class Map>
    bool encode_dictionary_structure(
        pb_ostream_t* stream,
        const pb_field_t* field,
        void* const* arg)
    {
        auto* state = static_cast<const dictionary_structure_encode_state<Map>*>(*arg);
        if (!state || !state->value)
            return false;

        for (const auto& entry : *state->value)
        {
            std::vector<char> value_buffer;
            entry.second.nanopb_serialise(value_buffer);

            pb_ostream_t size_stream = PB_OSTREAM_SIZING;
            if (!encode_length_delimited_field(&size_stream, 1, entry.first.data(), entry.first.size()))
                return false;
            if (!encode_length_delimited_field(&size_stream, 2, value_buffer.data(), value_buffer.size()))
                return false;

            std::vector<char> entry_buffer(size_stream.bytes_written);
            pb_ostream_t entry_stream
                = pb_ostream_from_buffer(reinterpret_cast<pb_byte_t*>(entry_buffer.data()), entry_buffer.size());
            if (!encode_length_delimited_field(&entry_stream, 1, entry.first.data(), entry.first.size()))
                return false;
            if (!encode_length_delimited_field(&entry_stream, 2, value_buffer.data(), value_buffer.size()))
                return false;

            if (!pb_encode_tag_for_field(stream, field))
                return false;
            if (!pb_encode_string(stream, reinterpret_cast<const pb_byte_t*>(entry_buffer.data()), entry_buffer.size()))
                return false;
        }

        return true;
    }

    template<class Map>
    bool encode_dictionary_submessage_entry(
        pb_ostream_t* stream,
        const std::string& key,
        const typename std::remove_reference_t<Map>::mapped_type& value,
        const dictionary_submessage_encode_state<Map>& state)
    {
        if (!encode_length_delimited_field(stream, 1, key.data(), key.size()))
            return false;

        if (!pb_encode_tag(stream, PB_WT_STRING, 2))
            return false;
        return state.encode_value(stream, state.value_fields, value);
    }

    template<class Map>
    bool encode_dictionary_submessage(
        pb_ostream_t* stream,
        const pb_field_t* field,
        void* const* arg)
    {
        auto* state = static_cast<const dictionary_submessage_encode_state<Map>*>(*arg);
        if (!state || !state->value || !state->value_fields || !state->encode_value)
            return false;

        for (const auto& entry : *state->value)
        {
            pb_ostream_t size_stream = PB_OSTREAM_SIZING;
            if (!encode_dictionary_submessage_entry<Map>(&size_stream, entry.first, entry.second, *state))
                return false;

            if (!pb_encode_tag_for_field(stream, field))
                return false;
            if (!pb_encode_varint(stream, size_stream.bytes_written))
                return false;
            if (!encode_dictionary_submessage_entry<Map>(stream, entry.first, entry.second, *state))
                return false;
        }

        return true;
    }

    template<class Map>
    bool decode_dictionary_structure(
        pb_istream_t* stream,
        const pb_field_t* field,
        void** arg)
    {
        (void)field;
        auto* state = static_cast<dictionary_structure_decode_state<Map>*>(*arg);
        if (!state || !state->value)
            return false;

        using value_type = typename std::remove_reference_t<Map>::mapped_type;
        std::string key;
        value_type value;

        while (stream->bytes_left)
        {
            pb_wire_type_t wire_type;
            uint32_t tag = 0;
            bool eof = false;
            if (!pb_decode_tag(stream, &wire_type, &tag, &eof))
                return eof;
            if (eof)
                break;

            if (wire_type != PB_WT_STRING)
                return false;

            if (tag == 1)
            {
                if (!read_length_delimited_string(stream, key))
                    return false;
            }
            else if (tag == 2)
            {
                std::vector<char> value_buffer;
                if (!read_length_delimited_bytes(stream, value_buffer))
                    return false;
                value.nanopb_deserialise(value_buffer);
            }
            else if (!pb_skip_field(stream, wire_type))
            {
                return false;
            }
        }

        (*state->value)[std::move(key)] = std::move(value);
        return true;
    }

    inline void encode_message(
        std::vector<char>& buffer,
        const pb_msgdesc_t* fields,
        const void* message)
    {
        constexpr size_t initial_encode_capacity = 256;
        size_t capacity = buffer.capacity();
        if (capacity < initial_encode_capacity)
            capacity = initial_encode_capacity;

        for (;;)
        {
            buffer.resize(capacity);
            pb_ostream_t stream = pb_ostream_from_buffer(reinterpret_cast<pb_byte_t*>(buffer.data()), buffer.size());
            if (pb_encode(&stream, fields, message))
            {
                buffer.resize(stream.bytes_written);
                return;
            }

            if (stream.errmsg == nullptr || std::string(stream.errmsg) != "stream full")
                throw std::runtime_error("Failed to encode nanopb message");

            capacity *= 2;
        }
    }

    inline void decode_message(
        const uint8_t* data,
        size_t size,
        const pb_msgdesc_t* fields,
        void* message)
    {
        pb_istream_t stream = pb_istream_from_buffer(reinterpret_cast<const pb_byte_t*>(data), size);
        if (!pb_decode(&stream, fields, message))
            throw std::runtime_error("Failed to decode nanopb message");
    }

    inline void decode_message(
        const std::vector<char>& buffer,
        const pb_msgdesc_t* fields,
        void* message)
    {
        decode_message(reinterpret_cast<const uint8_t*>(buffer.data()), buffer.size(), fields, message);
    }
}
