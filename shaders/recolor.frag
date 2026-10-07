#version 440

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 lightColor;   // what white paper becomes (theme background)
    vec4 darkColor;    // what black ink becomes (theme foreground)
    float keepHue;     // 0 = pure two-tone, 1 = keep the original hues
    float paperAlpha;  // opacity of the paper; ink stays fully opaque
};
layout(binding = 1) uniform sampler2D source;

void main()
{
    vec4 src = texture(source, qt_TexCoord0);
    vec3 rgb = src.a > 0.0 ? src.rgb / src.a : src.rgb;
    float lum = dot(rgb, vec3(0.2126, 0.7152, 0.0722));

    vec3 mapped = mix(darkColor.rgb, lightColor.rgb, lum);
    // Carry the original chroma over the remapped tone, so images and
    // coloured text keep their hues (zathura's recolor-keephue).
    mapped += (rgb - vec3(lum)) * keepHue * 0.9;
    mapped = clamp(mapped, 0.0, 1.0);

    float a = mix(1.0, paperAlpha, smoothstep(0.35, 0.95, lum)) * src.a * qt_Opacity;
    fragColor = vec4(mapped * a, a);
}
