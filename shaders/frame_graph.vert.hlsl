// Draws the frame graph with no vertex buffers: every corner comes from SV_VertexID, and the
// bar heights from a uniform. Vertices come in groups of six, one group per rectangle:
//   0 to 119    frame time bars, oldest first
//   120 to 239  input to paint bars, oldest first
//   240, 241    each strip's limit line
// See render/frame_graph.hpp for the uniform's layout, which sets all the sizes.

static const uint kBarsPerStrip = 120;

[[vk::binding(0, 1)]] cbuffer Graph
{
    float2 viewport_px;
    float bar_width_px;
    float strip_height_px;
    float margin_px;
    float strip_gap_px;
    float line_height_px;
    float limit_position; // the fraction of a strip's height at which the limit line sits
    float4 values[60];    // 240 floats, relative to each strip's limit
};

struct Output
{
    float4 position : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
};

Output main(uint vertex : SV_VertexID)
{
    static const float2 corners[6] = {
        float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
        float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
    };
    const float2 corner = corners[vertex % 6];
    const uint rectangle = vertex / 6;

    const bool is_line = rectangle >= 2 * kBarsPerStrip;
    const uint strip = is_line ? rectangle - (2 * kBarsPerStrip) : rectangle / kBarsPerStrip;
    const float strip_bottom = margin_px + (float(strip) * (strip_height_px + strip_gap_px));

    float2 low;
    float2 size;
    float4 color;
    if (is_line)
    {
        low = float2(margin_px, strip_bottom + (limit_position * strip_height_px));
        size = float2(float(kBarsPerStrip) * bar_width_px, line_height_px);
        color = float4(1.0, 1.0, 1.0, 1.0);
    }
    else
    {
        const uint bar = rectangle % kBarsPerStrip;
        const float value = values[rectangle / 4][rectangle % 4];
        // A negative value means no sample, which collapses to nothing. Values well over the
        // limit are clipped to the strip's height.
        const float height = value < 0.0 ? 0.0 : min(value * limit_position, 1.0) * strip_height_px;
        low = float2(margin_px + (float(bar) * bar_width_px), strip_bottom);
        size = float2(bar_width_px - 1.0, height);
        color = value > 1.0 ? float4(0.9, 0.15, 0.1, 1.0)
            : (strip == 0 ? float4(0.2, 0.8, 0.3, 1.0) : float4(0.25, 0.55, 0.95, 1.0));
    }

    const float2 pixel = low + (corner * size);
    Output output;
    output.position = float4(((pixel / viewport_px) * 2.0) - 1.0, 0.0, 1.0);
    output.color = color;
    return output;
}
