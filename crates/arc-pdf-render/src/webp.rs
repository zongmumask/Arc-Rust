//! WebP compression helpers for the preview/thumbnail render caches.
//!
//! ArcPDFKit stores preview and thumbnail bitmaps as WebP to keep memory
//! bounded; tile bitmaps stay raw. The available pure-Rust codec only offers
//! lossless VP8L encoding, so this module intentionally does not expose a
//! quality knob; lossy cache parity remains a future improvement.

use image::codecs::webp::{WebPDecoder, WebPEncoder};
use image::ExtendedColorType;

/// Encode RGBA pixels to lossless WebP.
pub fn encode_rgba(pixels: &[u8], width: u32, height: u32) -> Result<Vec<u8>, String> {
    if pixels.len() as u64 != width as u64 * height as u64 * 4 {
        return Err("invalid RGBA buffer size".to_string());
    }
    let mut out = Vec::new();
    WebPEncoder::new_lossless(&mut out)
        .encode(pixels, width, height, ExtendedColorType::Rgba8)
        .map_err(|e| e.to_string())?;
    Ok(out)
}

/// Decode WebP bytes back to RGBA.
pub fn decode_rgba(data: &[u8]) -> Result<(u32, u32, Vec<u8>), String> {
    let mut decoder = WebPDecoder::new(std::io::Cursor::new(data)).map_err(|e| e.to_string())?;
    let image = image::DynamicImage::from_decoder(decoder)
        .map_err(|e| e.to_string())?
        .to_rgba8();
    Ok((image.width(), image.height(), image.into_raw()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn webp_round_trip() {
        let width = 4u32;
        let height = 2u32;
        let pixels: Vec<u8> = (0..width * height * 4).map(|i| (i % 255) as u8).collect();
        let encoded = encode_rgba(&pixels, width, height).expect("encode");
        assert!(!encoded.is_empty());
        let (w, h, decoded) = decode_rgba(&encoded).expect("decode");
        assert_eq!((w, h), (width, height));
        assert_eq!(decoded, pixels);
    }
}
