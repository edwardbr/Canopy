// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// websocket_handshake.h

#include <string>
#include <vector>
#include <string_view>
#include <algorithm>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

#include "websocket_handshake.h"

namespace websocket_demo
{
    namespace v1
    {
        // --- Helper function to calculate Sec-WebSocket-Accept ---
        // Requires OpenSSL: -lssl -lcrypto
        std::string calculate_ws_accept(std::string_view client_key)
        {
            std::string combined = std::string(client_key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

            unsigned char hash[SHA_DIGEST_LENGTH];
            SHA1(reinterpret_cast<const unsigned char*>(combined.c_str()), combined.size(), hash);

            BIO *bio, *b64;
            BUF_MEM* bufferPtr;

            b64 = BIO_new(BIO_f_base64());
            bio = BIO_new(BIO_s_mem());
            bio = BIO_push(b64, bio);

            BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL); // No newlines in base64 output
            BIO_write(bio, hash, SHA_DIGEST_LENGTH);
            BIO_flush(bio);
            BIO_get_mem_ptr(bio, &bufferPtr);
            BIO_set_close(bio, BIO_NOCLOSE);
            BIO_free_all(bio);

            std::string result(bufferPtr->data, bufferPtr->length);
            BUF_MEM_free(bufferPtr);
            return result;
        }

    }
}
