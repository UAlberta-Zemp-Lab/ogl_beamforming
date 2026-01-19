/* See LICENSE for license details. */
layout(std430, buffer_reference, buffer_reference_align = 8) restrict readonly buffer Input {
	uint32_t values[];
};

layout(std430, buffer_reference, buffer_reference_align = 8) restrict writeonly buffer Output {
	uint32_t values[];
};

void main()
{
	uint32_t word = gl_GlobalInvocationID.x;
	if (word < words)
		Output(output_data).values[word] = Input(input_data).values[word];
}
