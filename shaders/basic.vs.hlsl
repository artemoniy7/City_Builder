// Вершинный шейдер
struct VSInput
{
    float3 position : POSITION;
    float4 color    : COLOR;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float4 color    : COLOR;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    // Преобразование в clip space (пока просто передаём позицию)
    output.position = float4(input.position, 1.0f);
    output.color = input.color;
    return output;
}