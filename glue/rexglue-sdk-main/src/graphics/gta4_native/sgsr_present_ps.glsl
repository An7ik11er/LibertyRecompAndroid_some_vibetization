#version 450

layout(set = 0, binding = 0) uniform sampler2D source_image;

layout(push_constant) uniform HDRPresentConstants {
  ivec2 source_extent;
  ivec2 destination_extent;
  float hdr_headroom;
  uint output_mode;
  uint hdr_mode;
  float paper_white_nits;
  float peak_nits;
  float shoulder_start;
  float shoulder_power;
  float sgsr_sharpness;
} present_constants;

layout(location = 0) out vec4 output_color;

// ===== SGSR 1 ALGORITHM (Qualcomm) =====
#define OperationMode 1
#define EdgeThreshold 8.0/255.0

float fastLanczos2(float x) {
    float wA = x - 4.0;
    float wB = x * wA - wA;
    wA *= wA;
    return wB * wA;
}

vec2 weightY(float dx, float dy, float c, float std) {
    float x = ((dx * dx) + (dy * dy)) * 0.55 + clamp(abs(c) * std, 0.0, 1.0);
    float w = fastLanczos2(x);
    return vec2(w, w * c);
}
// =======================================

// ===== HDR & DITHER LOGIC (from hdr_present_ps.glsl) =====
float luma(vec3 color) {
  return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

vec3 linear_to_srgb(vec3 color) {
  bvec3 use_linear_segment = lessThanEqual(color, vec3(0.0031308));
  vec3 linear_segment = color * 12.92;
  vec3 power_segment = 1.055 * pow(color, vec3(1.0 / 2.4)) - 0.055;
  return mix(power_segment, linear_segment, use_linear_segment);
}

vec3 srgb_to_linear(vec3 color) {
  bvec3 use_linear_segment = lessThanEqual(color, vec3(0.04045));
  vec3 linear_segment = color / 12.92;
  vec3 power_segment = pow((color + 0.055) / 1.055, vec3(2.4));
  return mix(power_segment, linear_segment, use_linear_segment);
}

float display_dither(ivec2 coordinate) {
  float noise = fract(dot(vec2(coordinate), vec2(0.75487766, 0.56984029)));
  return noise * (1.0 / 255.0) - (0.5 / 255.0);
}

vec3 dither_encoded_color(vec3 color, ivec2 coordinate) {
  return clamp(color + vec3(display_dither(coordinate)), vec3(0.0), vec3(1.0));
}

vec3 apply_auto_hdr(vec3 linear_color) {
  const float sdr_white_nits = 80.0;
  float paper_white = present_constants.paper_white_nits / sdr_white_nits;
  vec3 color = linear_color * paper_white;
  color /= paper_white;

  float sdr_ratio = luma(color);
  float auto_hdr_max_white = max(present_constants.peak_nits / (present_constants.paper_white_nits / sdr_white_nits), sdr_white_nits) / sdr_white_nits;
  
  if (sdr_ratio > present_constants.shoulder_start && present_constants.shoulder_start < 1.0 && sdr_ratio > 0.000001) {
    float shoulder_ratio = 1.0 - (max(1.0 - sdr_ratio, 0.0) / (1.0 - present_constants.shoulder_start));
    float extra_ratio = pow(clamp(shoulder_ratio, 0.0, 1.0), present_constants.shoulder_power) * (auto_hdr_max_white - 1.0);
    float total_ratio = sdr_ratio + extra_ratio;
    color *= total_ratio / sdr_ratio;
  }
  return color * paper_white;
}

vec3 sanitize_hdr_color(vec3 color) {
  color = mix(color, vec3(0.0), isnan(color));
  return clamp(color, vec3(0.0), vec3(65504.0));
}
// =========================================================

void main() {
  // В нативном проходе нет in_TEXCOORD0 (используется fullscreen triangle),
  // поэтому вычисляем UV из gl_FragCoord.
  vec2 uv = gl_FragCoord.xy / vec2(present_constants.destination_extent);
  vec2 src_extent = vec2(present_constants.source_extent);
  vec2 inv_src_extent = 1.0 / src_extent;

  // 1. Получаем базовый цвет через SGSR
  vec4 color;
  color.xyz = textureLod(source_image, uv, 0.0).xyz;

  highp vec2 imgCoord = ((uv * src_extent) + vec2(-0.5, 0.5));
  highp vec2 imgCoordPixel = floor(imgCoord);
  highp vec2 coord = (imgCoordPixel * inv_src_extent);
  vec2 pl = (imgCoord + (-imgCoordPixel));
  
  vec4 left = textureGather(source_image, coord, 1);
  float edgeVote = abs(left.z - left.y) + abs(color.y - left.y) + abs(color.y - left.z);

  if (edgeVote > EdgeThreshold) {
    coord.x += inv_src_extent.x;
    vec4 right = textureGather(source_image, coord + vec2(inv_src_extent.x, 0.0), 1);
    vec4 upDown;
    upDown.xy = textureGather(source_image, coord + vec2(0.0, -inv_src_extent.y), 1).wz;
    upDown.zw = textureGather(source_image, coord + vec2(0.0, inv_src_extent.y), 1).yx;

    float mean = (left.y + left.z + right.x + right.w) * 0.25;
    left = left - vec4(mean);
    right = right - vec4(mean);
    upDown = upDown - vec4(mean);
    color.w = color.y - mean;

    float sum = (((((abs(left.x)+abs(left.y))+abs(left.z))+abs(left.w))+(((abs(right.x)+abs(right.y))+abs(right.z))+abs(right.w)))+(((abs(upDown.x)+abs(upDown.y))+abs(upDown.z))+abs(upDown.w)));
    float std = 2.181818 / max(sum, 0.000001);

    vec2 aWY = weightY(pl.x, pl.y + 1.0, upDown.x, std);
    aWY += weightY(pl.x - 1.0, pl.y + 1.0, upDown.y, std);
    aWY += weightY(pl.x - 1.0, pl.y - 2.0, upDown.z, std);
    aWY += weightY(pl.x, pl.y - 2.0, upDown.w, std);
    aWY += weightY(pl.x + 1.0, pl.y - 1.0, left.x, std);
    aWY += weightY(pl.x, pl.y - 1.0, left.y, std);
    aWY += weightY(pl.x, pl.y, left.z, std);
    aWY += weightY(pl.x + 1.0, pl.y, left.w, std);
    aWY += weightY(pl.x - 1.0, pl.y - 1.0, right.x, std);
    aWY += weightY(pl.x - 2.0, pl.y - 1.0, right.y, std);
    aWY += weightY(pl.x - 2.0, pl.y, right.z, std);
    aWY += weightY(pl.x - 1.0, pl.y, right.w, std);

    float finalY = aWY.y / aWY.x;
    float maxY = max(max(left.y, left.z), max(right.x, right.w));
    float minY = min(min(left.y, left.z), min(right.x, right.w));
    finalY = clamp(present_constants.sgsr_sharpness * finalY, minY, maxY);

    float deltaY = finalY - color.w;
    deltaY = clamp(deltaY, -23.0 / 255.0, 23.0 / 255.0);

    color.x = clamp((color.x + deltaY), 0.0, 1.0);
    color.y = clamp((color.y + deltaY), 0.0, 1.0);
    color.z = clamp((color.z + deltaY), 0.0, 1.0);
  }

  // 2. Прогоняем результат через вашу логику HDR / Dither / Output
  bool source_is_srgb_encoded = (present_constants.output_mode & 4u) != 0u;
  bool hdr_enabled = present_constants.hdr_mode != 0u;
  bool output_dither_enabled = (present_constants.output_mode & 8u) != 0u && !hdr_enabled;
  
  vec3 source_color = max(color.rgb, vec3(0.0));
  vec3 encoded_source_color = clamp(source_color, vec3(0.0), vec3(1.0));
  ivec2 dest_coord = ivec2(gl_FragCoord.xy);

  if (source_is_srgb_encoded && output_dither_enabled) {
    encoded_source_color = dither_encoded_color(encoded_source_color, dest_coord);
  }

  vec3 linear_color = source_is_srgb_encoded ? srgb_to_linear(encoded_source_color) : source_color;

  if (hdr_enabled) {
    const float sdr_white_nits = 80.0;
    if (present_constants.hdr_mode == 2u) {
      linear_color = apply_auto_hdr(linear_color);
    } else {
      linear_color *= present_constants.paper_white_nits / sdr_white_nits;
    }
    linear_color = sanitize_hdr_color(linear_color);
    
    float configured_headroom = max(1.0, present_constants.peak_nits / sdr_white_nits);
    float effective_headroom = min(max(1.0, present_constants.hdr_headroom), configured_headroom);
    float maximum_component = max(max(linear_color.r, linear_color.g), linear_color.b);
    if (maximum_component > effective_headroom) {
      linear_color *= effective_headroom / maximum_component;
    }
    output_color = vec4(linear_color, 1.0);
  } else {
    vec3 sdr_color = source_is_srgb_encoded ? encoded_source_color : linear_to_srgb(clamp(linear_color, vec3(0.0), vec3(1.0)));
    if (!source_is_srgb_encoded && output_dither_enabled) {
      sdr_color = dither_encoded_color(sdr_color, dest_coord);
    }
    output_color = vec4(sdr_color, 1.0);
  }
}