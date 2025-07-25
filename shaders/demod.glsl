/* See LICENSE for license details. */
#if   defined(INPUT_DATA_TYPE_FLOAT)
  #define INPUT_DATA_TYPE       vec2
  #define OUTPUT_DATA_TYPE      vec2
  #define RF_SAMPLES_PER_INDEX  1
  #define RESULT_TYPE_CAST(v)   (v)
  #define SAMPLE_TYPE_CAST(v)   (v)
#else
  #define INPUT_DATA_TYPE       uint
  #define OUTPUT_DATA_TYPE      uint
  #define RF_SAMPLES_PER_INDEX  2
  #define RESULT_TYPE_CAST(v)   packSnorm2x16(v)
  #define SAMPLE_TYPE_CAST(v)   unpackSnorm2x16(v)
  /* NOTE(rnp): for outputting complex floats */
  //#define RESULT_TYPE_CAST(v)      (v)
  //#define SAMPLE_TYPE_CAST(v, i)   unpackSnorm2x16(v) * 32767.0f
#endif

layout(std430, binding = 1) readonly restrict buffer buffer_1 {
	INPUT_DATA_TYPE in_data[];
};

layout(std430, binding = 2) writeonly restrict buffer buffer_2 {
	OUTPUT_DATA_TYPE out_data[];
};

layout(r32f, binding = 0) readonly restrict uniform  image1D filter_coefficients;
layout(r16i, binding = 1) readonly restrict uniform iimage1D channel_mapping;

vec2 rotate_iq(vec2 iq, uint index)
{
	float arg    = radians(360) * demodulation_frequency * index / sampling_frequency;
	mat2  phasor = mat2( cos(arg), sin(arg),
	                    -sin(arg), cos(arg));
	vec2  result = phasor * iq;
	return result;
}

vec2 sample_rf(uint index)
{
	vec2 result = SAMPLE_TYPE_CAST(in_data[index]);
	return result;
}

void main()
{
	uint in_sample  = gl_GlobalInvocationID.x * decimation_rate;
	uint channel    = gl_GlobalInvocationID.y;
	uint transmit   = gl_GlobalInvocationID.z;

	uint in_channel = map_channels ? imageLoad(channel_mapping, int(channel)).x : channel;
	uint in_offset  = input_channel_stride * in_channel + input_transmit_stride * transmit;
	uint out_offset = output_channel_stride  * channel +
	                  output_transmit_stride * transmit +
	                  output_sample_stride   * gl_GlobalInvocationID.x;

	bool in_bounds;
	if (map_channels) {
		in_bounds = gl_GlobalInvocationID.x < output_channel_stride / output_sample_stride;
	} else {
		in_bounds = gl_GlobalInvocationID.x < output_transmit_stride;
	}

	if (in_bounds) {
		vec2  result = vec2(0);
		for (int i = 0; i < imageSize(filter_coefficients).x; i++) {
			uint index = in_sample + i * RF_SAMPLES_PER_INDEX;
			if (index < input_transmit_stride) {
				vec2 data = sample_rf((in_offset + index) / RF_SAMPLES_PER_INDEX) * vec2(1, -1);
				vec2 iq = sqrt(2.0f) * rotate_iq(data, index);
				result += imageLoad(filter_coefficients, imageSize(filter_coefficients).x - i - 1).x * iq;
			}
		}
		out_data[out_offset] = RESULT_TYPE_CAST(result);
	}
}
