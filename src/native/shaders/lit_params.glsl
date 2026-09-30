// =============================================================================
// lit_params.glsl -- per-draw parameters of the lit materials
// =============================================================================
// Shared by lit.* (the lit "simple" material), character.* (the character
// material) and reflect.* (reflective surfaces). The same block as
// native::LitParams in src/native/frame.h (read that for where each value
// comes from in the game): only vec4s and mat4s, so std140 lays it out
// exactly like the C++ struct. One block per draw, in a per-frame uniform
// buffer (1024 bytes each).
// =============================================================================

layout(set = 0, binding = 9, std140) uniform LitParams {
  mat4 mvp;       // model-view-projection (D3D conventions)
  mat4 world;     // model -> world
  mat4 rotation;  // model -> world for directions (normals): the 3x3 part
  vec4 eye;       // camera position (xyz)
  vec4 ambient;   // the scene's ambient light
  vec4 light_direction[4];  // the way each directional light travels
  vec4 light_colour[4];
  vec4 surface_diffuse;
  vec4 surface_specular;  // character: .x = amount
  vec4 surface_emissive;  // lit simple: added light (.w: added alpha)
  vec4 surface_ambient;
  vec4 specular_power;    // .x
  vec4 point_position[4];
  vec4 point_colour[4];
  vec4 point_params[4];   // x brightness, y radius, z intensity
  vec4 shadow_spheres[8];
  vec4 rim_colour;         // PS c100: character rim colour / reflect "EnvColour"
  vec4 character;         // x pulse, y blend weight, z rim blend, w rim amount
  vec4 fog_colour;
  vec4 fog_fade_alpha;    // x fog start, y fog end, z fade, w alpha test reference
  vec4 texture_sizes;     // xy diffuse, zw normal map (texels; palettized filtering)
  uvec4 counts;           // x directional lights on (bits), y point lights, z shadow spheres, w flags
  uvec4 more;             // x alpha test compare function (Xbox numbering)
} lit;

// counts.w (frame.h, kLit*).
const uint kLitTextured = 1u;
const uint kLitPalettized = 2u;
const uint kLitNormalPalettized = 4u;
const uint kLitFog = 8u;
const uint kLitFadeRgb = 16u;
const uint kLitAlphaTest = 32u;
const uint kLitBlendMap = 64u;
const uint kLitNoUv = 128u;
const uint kLitNoNormal = 256u;
const uint kLitNoTangents = 512u;
const uint kLitNoNormalMap = 1024u;
const uint kLitVertexColour = 2048u;  // reflect: use the vertex colour (the game's b10)
const uint kLitNoColour = 4096u;      // the vertex has no colour: white

bool LitFlag(uint flag) { return (lit.counts.w & flag) != 0u; }
