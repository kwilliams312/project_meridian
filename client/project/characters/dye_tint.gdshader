// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — dye mask-tint spatial shader (story #570, spec ⑤ §4 /
// design ① §6). A PARAMETERIZED VARIANT of the Character master material, NOT a
// new master (Art PRD §2.3 keeps the game-wide master count ≤ 12): the whole
// dye system is one shader driven entirely by per-asset uniform overrides — the
// assembler binds the mask + up to three channel colours at equip time.
//
// THE MODEL (① §6): every dyeable piece ships ONE RGB dye mask texture whose
// channels select regions — R = primary, G = secondary, B = accent. For each
// channel the artist marked (mask weight > 0), the masked albedo region is
// multiplied by that channel's chosen dye colour; every unmasked texel keeps the
// piece's AUTHORED albedo untouched. A channel with no dye chosen
// (use_<channel> = 0) is a no-op even where its mask is painted, so an
// undyed / partially-dyed piece renders exactly as authored.
//
// Base albedo is passed in explicitly (albedo_tex + albedo_color) rather than
// read from a StandardMaterial3D, because material_override REPLACES the mesh
// material — the assembler copies the piece's authored albedo across so the
// unmasked regions are pixel-identical to the untinted piece.
shader_type spatial;
render_mode cull_back, diffuse_burley, specular_schlick_ggx;

// Authored base albedo (the untinted piece). albedo_tex defaults to opaque white
// so a colour-only piece (no texture) is driven purely by albedo_color.
uniform sampler2D albedo_tex : source_color, hint_default_white;
uniform vec4 albedo_color : source_color = vec4(1.0, 1.0, 1.0, 1.0);

// The piece's RGB dye mask (R/G/B = primary/secondary/accent regions). Defaults
// to black = "no region dyeable", so a missing mask tints nothing.
uniform sampler2D dye_mask : source_color, hint_default_black;

// Per-channel chosen dye colour. Multiplied into the masked region so a dye
// darkens/hues the authored value rather than flat-replacing it (keeps painted
// value structure — Art PRD restyle §168 (2)).
uniform vec4 dye_primary : source_color = vec4(1.0, 1.0, 1.0, 1.0);
uniform vec4 dye_secondary : source_color = vec4(1.0, 1.0, 1.0, 1.0);
uniform vec4 dye_accent : source_color = vec4(1.0, 1.0, 1.0, 1.0);

// Whether each channel actually has a dye applied (0 = untinted, authored colour
// shows through even where the mask is painted). The assembler sets these from
// the wire dyes[] — an unknown dye id stays 0 (authored-colour fallback, ① §9).
uniform float use_primary : hint_range(0.0, 1.0) = 0.0;
uniform float use_secondary : hint_range(0.0, 1.0) = 0.0;
uniform float use_accent : hint_range(0.0, 1.0) = 0.0;

uniform float roughness : hint_range(0.0, 1.0) = 0.8;
// Per-piece metalness. Applied only to the piece's DYEABLE region (masked by the
// dye mask's coverage in fragment()), so unmasked texels stay matte — see the
// METALLIC line below.
uniform float metallic : hint_range(0.0, 1.0) = 0.0;

void fragment() {
	vec4 base = texture(albedo_tex, UV) * albedo_color;
	vec3 mask = texture(dye_mask, UV).rgb;
	vec3 col = base.rgb;

	// One masked multiply per channel. weight = mask region * use flag, so an
	// unpainted texel (mask 0) or an undyed channel (use 0) leaves `col` at the
	// authored value; a fully-masked, dyed texel becomes base * dye_colour.
	col = mix(col, col * dye_primary.rgb, mask.r * use_primary);
	col = mix(col, col * dye_secondary.rgb, mask.g * use_secondary);
	col = mix(col, col * dye_accent.rgb, mask.b * use_accent);

	// Metalness is LOCALIZED to the dyeable region (⑤/C4, #755): gate the
	// per-piece `metallic` by the mask's coverage (max of the R/G/B channels) so
	// unmasked texels stay matte. On the chibi body only R (skin) is painted, so a
	// metallic race (Gold/Silver) makes the SKIN metallic while the eyes + cloth
	// wrap (mask 0) stay matte. Armor masks cover their full surface with no gaps
	// (max == 1 everywhere) so whole-piece metalness is unchanged; a flat piece
	// (metallic 0) is a no-op. `metallic` keeps its whole-material meaning — the
	// mask only says WHERE it applies.
	float metallic_region = max(mask.r, max(mask.g, mask.b));

	ALBEDO = col;
	ROUGHNESS = roughness;
	METALLIC = metallic * metallic_region;
	ALPHA = base.a;
}
