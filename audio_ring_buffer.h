// Ring buffer tailored to audio buffering. Supports non-power-of-two sizes -
// it's not worth micro-optimizing here since almost all the work is in the
// memcpy()s.

template<size_t LENGTH>
class Audio_ring_buffer {
public:
    Audio_ring_buffer();

    // Writes up to 'len' samples from 'samples' to the ring buffer. In case of
    // overflow, writes as many samples as possible and returns 'false'.
    bool write_samples(int16_t const*samples, size_t len);

    // Moves up to 'len' samples from the ring buffer to 'out'. In case of
    // underflow, moves all remaining samples, zeroes the remainder of 'out'
    // (required by SDL2), and returns 'false'.
    bool read_samples(int16_t *out, size_t len);

    // Returns the fill level of the ring buffer as a double in the range
    // 0.0-1.0.
    double fill_level() const;

private:
    int16_t buf[LENGTH];
    // Indices from start_index up to but not including end_index (modulo
    // wrapping) contain samples
    size_t start_index, end_index;
    // True if the last operation was a read. Indicates whether
    // start_index == end_index means the buffer is full or empty.
    bool prev_op_was_read;
};

template<size_t LENGTH>
Audio_ring_buffer<LENGTH>::Audio_ring_buffer() :
  start_index(0), end_index(0), prev_op_was_read(true) {}

template<size_t LENGTH>
bool Audio_ring_buffer<LENGTH>::write_samples(int16_t const*samples, size_t len) {
    // Copy samples from 'samples' to the ring buffer by memcpy()ing contiguous
    // segments

    // How many contiguous bytes are available...?

    size_t contig_avail;
    if (start_index < end_index || (start_index == end_index && prev_op_was_read))
        contig_avail = LENGTH - end_index;
    else
        contig_avail = start_index - end_index;

    prev_op_was_read = false;

    if (contig_avail >= len) {
        // ...as many as we need. Copy it all in one go.
        memcpy(buf + end_index, samples, sizeof(int16_t)*len);
        end_index = (end_index + len) % LENGTH;
    }
    else {
        // ...less than we need. Fill the contiguous segment first.
        memcpy(buf + end_index, samples, sizeof(int16_t)*contig_avail);
        len -= contig_avail;
        assert(len > 0);
        // Move past the contiguous segment - possibly to index 0
        end_index = (end_index + contig_avail) % LENGTH;
        assert(end_index <= start_index);
        // How many contiguous bytes are available now...?
        size_t const avail = start_index - end_index;
        if (avail >= len) {
            // ...as many as we need. Copy the rest.
            memcpy(buf + end_index, samples + contig_avail, sizeof(int16_t)*len);
            end_index += len;
            assert(end_index <= start_index);
        }
        else {
            // ...less than we need. Copy as much as we can and drop the
            // rest.
            memcpy(buf + end_index, samples + contig_avail, sizeof(int16_t)*avail);
            assert(end_index + avail == start_index);
            end_index = start_index;
            // Overflow!
            return false;
        }
    }
    return true;
}

template<size_t LENGTH>
bool Audio_ring_buffer<LENGTH>::read_samples(int16_t *out, size_t len) {
    assert(start_index < LENGTH);

    // Move samples from the ring buffer to 'stream' by memcpy()ing contiguous
    // segments

    // How many contiguous bytes are available...?

    size_t contig_avail;
    if ((start_index == end_index && !prev_op_was_read) || start_index > end_index)
        contig_avail = LENGTH - start_index;
    else
        contig_avail = end_index - start_index;

    prev_op_was_read = true;

    if (contig_avail >= len) {
        // ...as many as we need. Copy it all in one go.
        memcpy(out, buf + start_index, sizeof(int16_t)*len);
        start_index = (start_index + len) % LENGTH;
    }
    else {
        // ... less than we need. Copy the contiguous segment first.
        memcpy(out, buf + start_index, sizeof(int16_t)*contig_avail);
        len -= contig_avail;
        assert(len > 0);
        // Move past the contiguous segment - possibly to index 0
        start_index = (start_index + contig_avail) % LENGTH;
        assert(start_index <= end_index);
        // How many contiguous bytes are available now...?
        size_t const avail = end_index - start_index;
        if (avail >= len) {
            // ...as many as we need. Copy the rest.
            memcpy(out + contig_avail, buf + start_index, sizeof(int16_t)*len);
            start_index += len;
            assert(start_index <= end_index);
        }
        else {
            // ...less than we need. Copy as much as we can and zero-fill
            // the rest of the output buffer, as required by SDL2.
            memcpy(out + contig_avail, buf + start_index, sizeof(int16_t)*avail);
            memset(out + contig_avail + avail, 0, sizeof(int16_t)*(len - avail));
            assert(start_index + avail == end_index);
            start_index = end_index;
            // Underflow!
            return false;
        }
    }
    return true;
}

template<size_t LENGTH>
double Audio_ring_buffer<LENGTH>::fill_level() const {
    double const data_len =
      (end_index + LENGTH - start_index) % LENGTH;
    return data_len/LENGTH;
}
