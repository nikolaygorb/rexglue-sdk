// Direct3D 11 only (built for Shader Model 5.0 alone, see build_shaders.py).
// Direct3D 11 can't copy a buffer to a texture, so texels decoded by the
// texture load shaders (rows of host texels in a raw buffer) are stored into
// the texture through a UINT view of its typeless format. Each component is
// taken from the source texel's bits and stored as the integer of the view's
// format, so the normalized, signed and float views of the texture see the
// same bits. Components of narrower sources (16-bit 5:6:5, 1:5:5:5, 4:4:4:4
// and 6:5:5 texels, for which Direct3D 11 has no typed UAV) are widened by
// repeating their bits, which gives the same normalized value as the source
// up to the rounding of the wider format.

ByteAddressBuffer xe_texture_upload_source : register(t0);
#if XE_TEXTURE_UPLOAD_3D
RWTexture3D<uint4> xe_texture_upload_dest : register(u0);
#else
RWTexture2DArray<uint4> xe_texture_upload_dest : register(u0);
#endif

cbuffer XeTextureUploadConstants : register(b0) {
  // Byte offset of the first texel in the source.
  uint xe_texture_upload_offset;
  uint xe_texture_upload_row_pitch;
  // Between 3D slices or array layers.
  uint xe_texture_upload_slice_pitch;
  uint xe_texture_upload_texel_bytes;
  // Width, height, and depth or layer count, in texels.
  uint3 xe_texture_upload_size;
  // The first destination 3D slice or array layer.
  uint xe_texture_upload_dest_z;
  // Per component: source bit offset in bits 0:7, source bit count in 8:15
  // (0 for all ones), destination bit count in 16:23 (0 for an unused
  // component).
  uint4 xe_texture_upload_components;
};

uint XeTextureUploadReadBits(uint byte_address, uint bit_offset, uint bit_count) {
  uint bit_address = (byte_address & 3u) * 8u + bit_offset;
  uint dword_address = (byte_address & ~3u) + (bit_address >> 5u) * 4u;
  uint shift = bit_address & 31u;
  uint value = xe_texture_upload_source.Load(dword_address) >> shift;
  if (shift != 0u && shift + bit_count > 32u) {
    value |= xe_texture_upload_source.Load(dword_address + 4u) << (32u - shift);
  }
  return bit_count >= 32u ? value : (value & ((1u << bit_count) - 1u));
}

[numthreads(8, 8, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  [branch] if (any(xe_thread_id >= xe_texture_upload_size)) {
    return;
  }
  uint address = xe_texture_upload_offset + xe_thread_id.z * xe_texture_upload_slice_pitch +
                 xe_thread_id.y * xe_texture_upload_row_pitch +
                 xe_thread_id.x * xe_texture_upload_texel_bytes;
  uint4 value;
  [unroll] for (uint i = 0u; i < 4u; ++i) {
    uint component = xe_texture_upload_components[i];
    uint source_bits = (component >> 8u) & 0xFFu;
    uint dest_bits = (component >> 16u) & 0xFFu;
    uint dest_mask = dest_bits >= 32u ? 0xFFFFFFFFu : ((1u << dest_bits) - 1u);
    uint component_value;
    [branch] if (source_bits == 0u) {
      component_value = dest_mask;
    } else {
      component_value = XeTextureUploadReadBits(address, component & 0xFFu, source_bits);
      [branch] if (dest_bits > source_bits) {
        if (source_bits == 1u) {
          component_value = component_value != 0u ? dest_mask : 0u;
        } else {
          // Bit repetition, for at most twice the source bits (8 from 4+).
          component_value = (component_value << (dest_bits - source_bits)) |
                            (component_value >> (2u * source_bits - dest_bits));
        }
      }
    }
    value[i] = component_value;
  }
  xe_texture_upload_dest[uint3(xe_thread_id.xy, xe_thread_id.z + xe_texture_upload_dest_z)] =
      value;
}
