//! Watermark data model, ported from ArcPDFKit `Models/Watermark.swift`.
//!
//! Pure data with serde support; no PDFium or platform types. `flatten` is a
//! runtime-only flag and is deliberately excluded from serialization so old
//! presets without the field keep decoding.

use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum WatermarkContentType {
    Text,
    File,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum WatermarkSourceKind {
    Image,
    Pdf,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum WatermarkLayout {
    Single,
    Grid,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct WatermarkFont {
    pub postscript_name: String,
    pub rel_size: f64,
    pub bold: bool,
    pub italic: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub struct WatermarkColor {
    pub red: f64,
    pub green: f64,
    pub blue: f64,
    pub alpha: f64,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct WatermarkDefinition {
    pub id: String,
    pub content_type: WatermarkContentType,
    pub text: Option<String>,
    pub font: Option<WatermarkFont>,
    pub color: Option<WatermarkColor>,
    pub source_data: Option<Vec<u8>>,
    pub source_kind: Option<WatermarkSourceKind>,
    pub layout: WatermarkLayout,
    pub rotation: f64,
    pub opacity: f64,
    pub scale: f64,
    pub anchor_x: f64,
    pub anchor_y: f64,
    pub offset_x: f64,
    pub offset_y: f64,
    pub spacing: f64,
    pub on_top: bool,
    #[serde(skip)]
    pub flatten: bool,
}

impl<'de> Deserialize<'de> for WatermarkDefinition {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        #[derive(Deserialize)]
        struct Wire {
            id: String,
            #[serde(rename = "type")]
            content_type: WatermarkContentType,
            text: Option<String>,
            font: Option<WatermarkFont>,
            color: Option<WatermarkColor>,
            source_data: Option<Vec<u8>>,
            source_kind: Option<WatermarkSourceKind>,
            layout: WatermarkLayout,
            rotation: f64,
            opacity: f64,
            scale: f64,
            #[serde(default = "default_center")]
            anchor_x: f64,
            #[serde(default = "default_center")]
            anchor_y: f64,
            #[serde(default)]
            offset_x: f64,
            #[serde(default)]
            offset_y: f64,
            #[serde(default = "default_spacing")]
            spacing: f64,
            #[serde(default = "default_true")]
            on_top: bool,
        }

        fn default_center() -> f64 {
            0.5
        }
        fn default_spacing() -> f64 {
            4.0
        }
        fn default_true() -> bool {
            true
        }

        let wire = Wire::deserialize(deserializer)?;
        Ok(Self {
            id: wire.id,
            content_type: wire.content_type,
            text: wire.text,
            font: wire.font,
            color: wire.color,
            source_data: wire.source_data,
            source_kind: wire.source_kind,
            layout: wire.layout,
            rotation: wire.rotation,
            opacity: wire.opacity,
            scale: wire.scale,
            anchor_x: wire.anchor_x,
            anchor_y: wire.anchor_y,
            offset_x: wire.offset_x,
            offset_y: wire.offset_y,
            spacing: wire.spacing,
            on_top: wire.on_top,
            flatten: false,
        })
    }
}

impl Default for WatermarkDefinition {
    fn default() -> Self {
        Self {
            id: String::new(),
            content_type: WatermarkContentType::Text,
            text: None,
            font: None,
            color: None,
            source_data: None,
            source_kind: None,
            layout: WatermarkLayout::Single,
            rotation: 0.0,
            opacity: 1.0,
            scale: 1.0,
            anchor_x: 0.5,
            anchor_y: 0.5,
            offset_x: 0.0,
            offset_y: 0.0,
            spacing: 4.0,
            on_top: true,
            flatten: false,
        }
    }
}

impl WatermarkDefinition {
    pub fn file_source_is_image(&self) -> bool {
        self.source_kind == Some(WatermarkSourceKind::Image)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const LEGACY: &str = r#"{
        "id":"wm-1","type":"text","text":"Draft","layout":"grid",
        "rotation":45,"opacity":0.5,"scale":1.2
    }"#;

    #[test]
    fn legacy_json_gets_defaults_and_no_flatten() {
        let def: WatermarkDefinition = serde_json::from_str(LEGACY).expect("decode legacy preset");
        assert_eq!(def.anchor_x, 0.5);
        assert_eq!(def.anchor_y, 0.5);
        assert_eq!(def.spacing, 4.0);
        assert!(def.on_top);
        assert!(!def.flatten);
        assert_eq!(def.text.as_deref(), Some("Draft"));
        assert_eq!(def.layout, WatermarkLayout::Grid);
    }

    #[test]
    fn flatten_is_not_serialized() {
        let mut def: WatermarkDefinition = serde_json::from_str(LEGACY).unwrap();
        def.flatten = true;
        let json = serde_json::to_string(&def).unwrap();
        assert!(!json.contains("flatten"));
    }
}
