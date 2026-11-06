[[vk::binding(0, 2)]] Texture2D checker_texture;
[[vk::binding(0, 2)]] SamplerState checker_sampler;

float4 main([[vk::location(0)]] float2 uv : TEXCOORD0) : SV_Target0
{
    return checker_texture.Sample(checker_sampler, uv);
}
