#ifndef __CFFI_POINTER_STREAM_PEER_HPP__
#define __CFFI_POINTER_STREAM_PEER_HPP__

#include "core/io/stream_peer.h"

#include "cffi_pointer.hpp"


namespace cffi {

class StreamPeerCFFIPointer : public StreamPeerExtension {
	GDCLASS(StreamPeerCFFIPointer, StreamPeerExtension)
public:
	// The engine's StreamPeerExtension exposes GDVIRTUAL _get_data/_put_data for
	// script subclassing; a module overrides the public virtuals instead.
	Error get_data(uint8_t *r_buffer, int p_bytes) override;
	Error get_partial_data(uint8_t *r_buffer, int p_bytes, int &r_received) override;
	Error put_data(const uint8_t *p_data, int p_bytes) override;
	Error put_partial_data(const uint8_t *p_data, int p_bytes, int &r_sent) override;
	int get_available_bytes() const override;

	void set_pointer(Ref<CFFIPointer> pointer);
	Ref<CFFIPointer> get_pointer() const;

	void set_size_in_bytes(int size);
	int get_size_in_bytes() const;

	void set_size(int size);
	int get_size() const;

	void seek(int p_pos);
	int get_position() const;

	void clear();

protected:
	static void _bind_methods();

private:
	Ref<CFFIPointer> pointer = nullptr;
	int size_in_bytes = 0;
	int cursor = 0;
};

}

#endif  // __CFFI_POINTER_STREAM_PEER_HPP__