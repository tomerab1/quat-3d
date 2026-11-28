#include "engine/pack/digest.hpp"

#include <array>
#include <fstream>
#include <utility>
#include <vector>

#include <openssl/evp.h>

#include "engine/core/utf8_path.hpp"

namespace engine::pack {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

} // namespace

struct Sha256::Context {
    struct Free {
        void operator()(EVP_MD_CTX* c) const { EVP_MD_CTX_free(c); }
    };
    std::unique_ptr<EVP_MD_CTX, Free> md{EVP_MD_CTX_new()};
};

Sha256::Sha256() : ctx_(std::make_unique<Context>()) {
    ok_ = ctx_->md != nullptr && EVP_DigestInit_ex(ctx_->md.get(), EVP_sha256(), nullptr) == 1;
}

Sha256::~Sha256() = default;
Sha256::Sha256(Sha256&&) noexcept = default;
Sha256& Sha256::operator=(Sha256&&) noexcept = default;

void Sha256::update(std::string_view data) {
    if (ok_ && !data.empty()) {
        ok_ = EVP_DigestUpdate(ctx_->md.get(), data.data(), data.size()) == 1;
    }
}

std::expected<std::string, core::Error> Sha256::hex() {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    if (!ok_ || EVP_DigestFinal_ex(ctx_->md.get(), digest.data(), &size) != 1 || size != 32) {
        ok_ = false;
        return fail("SHA-256: OpenSSL digest failed");
    }
    ok_ = false; // spent
    constexpr char k_digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned int i = 0; i < size; ++i) {
        out.push_back(k_digits[digest[i] >> 4]);
        out.push_back(k_digits[digest[i] & 0xF]);
    }
    return out;
}

std::expected<std::string, core::Error> sha256_hex(std::string_view data) {
    Sha256 h;
    h.update(data);
    return h.hex();
}

std::expected<std::string, core::Error> sha256_file(const std::filesystem::path& path,
                                                    std::uint64_t& bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("cannot read '" + core::path_to_utf8(path) + "'");
    Sha256 h;
    std::vector<char> chunk(1U << 16);
    bytes = 0;
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got == 0) break;
        h.update(std::string_view(chunk.data(), got));
        bytes += got;
    }
    if (in.bad()) return fail("read error on '" + core::path_to_utf8(path) + "'");
    return h.hex();
}

} // namespace engine::pack
