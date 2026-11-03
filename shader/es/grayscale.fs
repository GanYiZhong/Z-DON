#version 300 es
precision mediump float;

in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;

uniform sampler2D texture0;
uniform float amount;

void main() {
    vec4 c = texture(texture0, fragTexCoord);
    float y = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    finalColor = vec4(mix(c.rgb, vec3(y), amount), c.a) * fragColor;
}
