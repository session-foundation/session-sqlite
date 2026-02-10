#pragma once
#include <cstddef>
#include <span>
#include <type_traits>

namespace session {

// Type used to provide a libsodium secure member buffer that is (if possible) protected from
// swapping or coredumps, is locked to be unreadable within the process when not in use, and is
// securely overwritten on destruction.
//
// See libsodium's Secure Memory documentation for details.
//
// Note that using this secure memory approach is considerably more computationally expensive than
// an ordinary buffer, and that the buffer always requires a minimum of 3 pages or RAM (which could
// be anywhere from 12kiB to 192kiB depending on architecture and OS).  This class is only
// recommended when you need a very small number of infrequently accessed, long-lived, secure
// buffers.
class secure_buffer {
    std::span<std::byte> buf;

    int accessors = 0, writers = 0;

  public:
    // RAII class that unlocks the buffer allowing it to be read (and optionally written) while
    // the accessor (or any duplicates of it) is alive.  Upon destruction of all active
    // accessors the memory is re-locked to be unreadable.
    template <bool ReadWrite = false>
    struct accessor {
      private:
        friend class secure_buffer;
        secure_buffer& owner;

        accessor(secure_buffer& c, bool);

      public:
        const std::span<std::conditional_t<ReadWrite, std::byte, const std::byte>> buf{owner.buf};

        accessor(secure_buffer& c) : accessor{c, false} {}
        ~accessor();

        // Copy constructible, but not copy assignable.  The copy counts as another accessor.
        accessor(const accessor&);
        accessor& operator=(const accessor&) = delete;
    };

    using r_accessor = accessor<false>;
    using rw_accessor = accessor<true>;

    // Returns an RAII class that provides read (or read-write) access to the buffer while the
    // accessor remains alive.  Attempting to read the pointed at memory after destruction of
    // the accessor will kill the program.
    r_accessor access() { return {*this}; }
    rw_accessor access_rw() { return {*this}; }

    // Returns the size of the buffer.
    size_t size() const { return buf.size(); }

    // Updates the buffer for the stored sensitive value to the given size, if it is not currently
    // the requested size, otherwise does nothing.  If `preserve` is true and a resize is
    // necessary, as much of the existing buffer as possible will be copied into the new buffer.
    // If false (or omitted) then this resize *does not* preserve data when the buffer size
    // changes.  Note that if the buffer size does not change, the existing data will not be
    // changed regardless of the value of `preserve` value.
    //
    // Note: There should be no active accessors of this buffer when this method is called.
    // That is, you should resize *before* obtaining a read-write access object, not after.
    //
    // This returns an rw_accessor as rw access is typically needed immediately after a resize (and
    // is required during the resize already); it can simply not be captured if not needed.
    rw_accessor resize(size_t new_size = 0, bool preserve = false);

    // Updates the stored sensitive value.  Equivalent to calling resize(), and then replacing the
    // buffer.  There should be no active accessors of this buffer when this is called.
    //
    // Returns an rw_accessor used during the update; this can simply be discarded if not needed.
    rw_accessor update(std::span<const std::byte> key);

    // Default constructor makes an instance without any allocated buffer.
    secure_buffer();

    // Allocates secure memory sufficient to hold `key`, writes the value into it, and then
    // makes it inaccessible to the process (except via `access()` or `update()`).
    secure_buffer(std::span<const std::byte> key);

    // Destroys the key value securely: first overwriting it with 0s, and then freeing it.
    // There should not be any lingering accessors when this is destroyed.
    ~secure_buffer();

    secure_buffer(const secure_buffer&) = delete;
    secure_buffer& operator=(const secure_buffer&) = delete;
    secure_buffer(secure_buffer&&) = delete;
    secure_buffer& operator=(secure_buffer&&) = delete;
};

extern template struct secure_buffer::accessor<true>;
extern template struct secure_buffer::accessor<false>;

}  // namespace session
