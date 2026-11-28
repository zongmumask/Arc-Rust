//! Small GPUI-native image wrappers shared by preview/tile painting.

use arc_pdf_engine::Raster;
use gpui::RenderImage;
use std::sync::Arc;

#[derive(Clone, Debug)]
pub struct RasterImage(pub Arc<RenderImage>);

impl RasterImage {
    pub fn from_raster(raster: &Raster) -> Self {
        let width = raster.width;
        let height = raster.height;
        let row_bytes = width * 4;
        let mut bytes = vec![0u8; width * height * 4];
        for y in 0..height {
            let src = y * raster.stride as usize;
            let dst = y * row_bytes;
            let count = (raster.stride as usize).min(row_bytes);
            bytes[dst..dst + count].copy_from_slice(&raster.pixels[src..src + count]);
        }
        let image = image::RgbaImage::from_raw(width as u32, height as u32, bytes)
            .expect("raster buffer size");
        Self(Arc::new(RenderImage::new(vec![image::Frame::new(image)])))
    }
}
