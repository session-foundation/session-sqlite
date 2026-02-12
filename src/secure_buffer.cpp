#include <sodium/core.h>
#include <sodium/utils.h>

#include <cassert>
#include <cstring>
#include <session/secure_buffer.hpp>
#include <stdexcept>

namespace session {

secure_buffer::secure_buffer() {
    if (sodium_init() == -1)
        throw std::runtime_error{"libsodium initialization failed"};
}

secure_buffer::secure_buffer(std::span<const std::byte> key) : secure_buffer{} {
    update(key);
}

secure_buffer::~secure_buffer() {
    assert(accessors == 0);
    if (buf.data())
        sodium_free(buf.data());
}

template <bool ReadWrite>
secure_buffer::accessor<ReadWrite>::accessor(secure_buffer& c, bool adopt) : owner{c} {
    owner.accessors++;
    if (ReadWrite)
        owner.writers++;

    if (!owner.buf.data() || adopt)
        return;

    if (ReadWrite && owner.writers == 1)  // first writer
        sodium_mprotect_readwrite(owner.buf.data());
    else if (owner.accessors == 1)  // first readonly accessor
        sodium_mprotect_readonly(owner.buf.data());
}

template <bool ReadWrite>
secure_buffer::accessor<ReadWrite>::accessor(const accessor& a) : owner{a.owner} {
    owner.accessors++;
    if (ReadWrite)
        owner.writers++;
}

template <bool ReadWrite>
secure_buffer::accessor<ReadWrite>::~accessor() {
    --owner.accessors;
    if (ReadWrite)
        --owner.writers;

    if (!owner.buf.data())
        return;

    if (owner.accessors == 0)  // last accessor, so lock it
        sodium_mprotect_noaccess(owner.buf.data());
    else if (ReadWrite && owner.writers == 0)  // last writer, so make it readonly
        sodium_mprotect_readonly(owner.buf.data());
}

template struct secure_buffer::accessor<true>;
template struct secure_buffer::accessor<false>;

secure_buffer::rw_accessor secure_buffer::resize(size_t new_size, bool preserve) {
    assert(accessors == 0);

    if (new_size == buf.size())
        return access_rw();

    if (new_size == 0) {
        sodium_free(buf.data());
        buf = {static_cast<std::byte*>(nullptr), 0};
        return access_rw();
    }

    auto* new_buf = static_cast<std::byte*>(sodium_malloc(new_size));
    if (!new_buf)
        throw std::bad_alloc{};

    auto old = buf;
    buf = {new_buf, new_size};

    auto acc = rw_accessor{*this, true};

    if (old.data()) {
        if (preserve && old.size() > 0) {
            // We don't actually need to write, but sodium_free is going to readwrite unlock it
            // anyway so we might as well:
            sodium_mprotect_readwrite(old.data());
            std::memcpy(buf.data(), old.data(), std::min(buf.size(), old.size()));
        }
        sodium_free(old.data());
    }

    return acc;
}

secure_buffer::rw_accessor secure_buffer::update(std::span<const std::byte> key) {
    auto rw = resize(key.size(), false);
    std::memcpy(rw.buf.data(), key.data(), key.size());
    return rw;
}

}  // namespace session
