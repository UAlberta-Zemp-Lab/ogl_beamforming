/* See LICENSE for license details. */
layout(local_size_x = 32, local_size_y = 1, local_size_z = 32) in;

layout(std430, binding = 1) readonly restrict buffer buffer_1 {
	vec2 rf_data[];
};

layout(rg32f, binding = 0) writeonly uniform image3D u_out_data_tex;

layout(location = 2) uniform int   u_volume_export_pass;
layout(location = 3) uniform ivec3 u_volume_export_dim_offset;
layout(location = 4) uniform mat4  u_xdc_transform;
layout(location = 5) uniform int   u_xdc_index;
layout(location = 6) uniform float u_cycle_t;

#define C_SPLINE 0.5

#define TX_ROWS 0
#define TX_COLS 1

#if 1
/* NOTE: interpolation is unnecessary if the data has been demodulated and not decimated */
vec2 cubic(uint ridx, float t)
{
	return rf_data[ridx + uint(floor(t))];
}
#else
/* NOTE: See: https://cubic.org/docs/hermite.htm */
vec2 cubic(uint ridx, float x)
{
	mat4 h = mat4(
		 2, -3,  0, 1,
		-2,  3,  0, 0,
		 1, -2,  1, 0,
		 1, -1,  0, 0
	);

	uint  xk = uint(floor(x));
	float t  = (x  - float(xk));
	vec4  S  = vec4(t * t * t, t * t, t, 1);

	vec2 P1 = rf_data[ridx + xk];
	vec2 P2 = rf_data[ridx + xk + 1];
	vec2 T1 = C_SPLINE * (P2 - rf_data[ridx + xk - 1]);
	vec2 T2 = C_SPLINE * (rf_data[ridx + xk + 2] - P1);

	vec4 C1 = vec4(P1.x, P2.x, T1.x, T2.x);
	vec4 C2 = vec4(P1.y, P2.y, T1.y, T2.y);
	return vec2(dot(S, h * C1), dot(S, h * C2));
}
#endif

vec3 calc_image_point(vec3 voxel)
{
	ivec3 out_data_dim = imageSize(u_out_data_tex);
	vec4 output_size   = abs(output_max_coord - output_min_coord);
	vec3 image_point   = output_min_coord.xyz + voxel * output_size.xyz / out_data_dim;

	switch (das_shader_id) {
	case DAS_ID_UFORCES:
		/* TODO: fix the math so that the image plane can be aritrary */
		image_point.y = 0;
		break;
	case DAS_ID_HERCULES:
	case DAS_ID_RCA:
		if (u_volume_export_pass == 0)
			image_point.y = off_axis_pos;
		break;
	}

	return image_point;
}

vec2 apodize(vec2 value, float apodization_arg, float distance)
{
	/* NOTE: apodization value for this transducer element */
	float a  = cos(clamp(abs(apodization_arg * distance), 0, 0.25 * radians(360)));
	return value * a * a;
}

vec3 row_column_point_scale(bool tx_rows)
{
	return vec3(float(!tx_rows), float(tx_rows), 1);
}

vec3 world_space_to_rca_space(vec3 image_point, int transmit_orientation)
{
	return (u_xdc_transform * vec4(image_point, 1)).xyz * row_column_point_scale(transmit_orientation != TX_ROWS);
}

float sample_index(float distance)
{
	float  time = distance / speed_of_sound + time_offset;
	return time * sampling_frequency;
}

float planewave_transmit_distance(vec3 point, float transmit_angle)
{
	return dot(point, vec3(sin(transmit_angle), sin(transmit_angle), cos(transmit_angle)));
}

vec2 RCA(vec3 image_point, vec3 delta, uint starting_offset, float apodization_arg)
{
	/* TODO: pass this in (there is a problem in that it depends on the orientation
	 * of the array relative to the target/subject). */
	int  transmit_orientation = TX_ROWS;
	uint ridx      = starting_offset;
	int  direction = beamform_plane * (u_volume_export_pass ^ 1);
	if (direction == TX_COLS) image_point = image_point.yxz;

	vec3 transmit_point = image_point * row_column_point_scale(transmit_orientation == TX_ROWS);
	vec3 recieve_point  = world_space_to_rca_space(image_point, transmit_orientation);
	// vec3  recieve_point  = (u_xdc_transform * vec4(image_point, 1)).xyz;

	vec2 sum = vec2(0);
	/* NOTE: For Each Acquistion in Raw Data */
	// uint i = (dec_data_dim.z - 1) * uint(clamp(u_cycle_t, 0, 1)); {
	for (uint i = 0; i < dec_data_dim.z; i++) {
		uint base_idx = i / 4;
		uint sub_idx  = i % 4;

		float focal_depth    = focal_depths[base_idx][sub_idx];
		float transmit_angle = transmit_angles[base_idx][sub_idx];
		float tdist;
		if (isinf(focal_depth)) {
			tdist = planewave_transmit_distance(transmit_point, transmit_angle);
		} else {
			vec3 f  = vec3(sin(transmit_angle), sin(transmit_angle), cos(transmit_angle));
			f      *= focal_depth * row_column_point_scale(transmit_orientation == TX_ROWS);
			tdist   = distance(transmit_point, f);
		}

		vec3 rdist = recieve_point;
		/* NOTE: For Each Receiver */
		// uint j = (dec_data_dim.z - 1) * uint(clamp(u_cycle_t, 0, 1)); {
		for (uint j = 0; j < dec_data_dim.y; j++) {
			float sidx  = sample_index(tdist + length(rdist));
			vec2 valid  = vec2(sidx < dec_data_dim.x);
			sum        += apodize(cubic(ridx, sidx), apodization_arg, rdist[0]) * valid;
			rdist[0]   -= delta[0];
			ridx       += dec_data_dim.x;
		}
	}
	return sum;
}

vec2 HERCULES(vec3 image_point, vec3 delta, uint starting_offset, float apodization_arg)
{
	/* TODO: pass this in (there is a problem in that it depends on the orientation
	 * of the array relative to the target/subject). */
	int   transmit_orientation = TX_ROWS;
	float focal_depth    = focal_depths[0][0];
	float transmit_angle = transmit_angles[0][0];
	vec3  transmit_point = image_point * row_column_point_scale(transmit_orientation == TX_ROWS);
	//vec3  recieve_point  = world_space_to_rca_space(image_point, transmit_orientation);
	vec3  recieve_point  = (u_xdc_transform * vec4(image_point, 1)).xyz;

	float tdist;
	if (isinf(focal_depth)) {
		tdist = planewave_transmit_distance(transmit_point, transmit_angle);
	} else {
		vec3 f  = vec3(sin(transmit_angle), sin(transmit_angle), cos(transmit_angle));
		f      *= focal_depth * row_column_point_scale(transmit_orientation == TX_ROWS);
		tdist   = distance(transmit_point, f);
	}

	uint ridx      = starting_offset;
	vec3 rdist     = recieve_point;
	int  direction = beamform_plane * (u_volume_export_pass ^ 1);

	vec2 sum = vec2(0);
	/* NOTE: For Each Acquistion in Raw Data */
	for (uint i = 0; i < dec_data_dim.z; i++) {
		/* NOTE: For Each Virtual Source */
		for (uint j = 0; j < dec_data_dim.y; j++) {
			float sidx = sample_index(tdist + length(rdist));
			vec2 valid = vec2(sidx >= 0) * vec2(sidx < dec_data_dim.x);

			/* NOTE: tribal knowledge */
			if (i == 0) valid *= inversesqrt(128);

			sum += apodize(cubic(ridx, sidx), apodization_arg, rdist.x) * valid;

			rdist[direction] -= delta[direction];
			ridx             += dec_data_dim.x;
		}

		rdist[direction]      = recieve_point[direction];
		rdist[direction ^ 1] -= delta[direction ^ 1];
	}
	return sum;
}

vec2 uFORCES(vec3 image_point, vec3 delta, float y_off, uint starting_offset, float apodization_arg)
{
	/* NOTE: skip first acquisition in uforces since its garbage */
	uint uforces = uint(dec_data_dim.y != dec_data_dim.z);
	uint ridx    = starting_offset + dec_data_dim.y * dec_data_dim.x * uforces;

	image_point  = (u_xdc_transform * vec4(image_point, 1)).xyz;

	vec2 sum = vec2(0);
	for (uint i = uforces; i < dec_data_dim.z; i++) {
		uint base_idx = (i - uforces) / 4;
		uint sub_idx  = (i - uforces) % 4;

		vec3  rdist         = image_point;
		vec3  focal_point   = uforces_channels[base_idx][sub_idx] * delta + vec3(0, y_off, 0);
		float transmit_dist = distance(image_point, focal_point);

		for (uint j = 0; j < dec_data_dim.y; j++) {
			float sidx  = sample_index(transmit_dist + length(rdist));
			vec2 valid  = vec2(sidx >= 0) * vec2(sidx < dec_data_dim.x);
			sum        += apodize(cubic(ridx, sidx), apodization_arg, rdist.x) * valid;
			rdist      -= delta;
			ridx       += dec_data_dim.x;
		}
	}
	return sum;
}

void main()
{

	/* NOTE: Convert voxel to physical coordinates */
	ivec3 out_coord    = ivec3(gl_GlobalInvocationID);
	vec3  image_point  = calc_image_point(vec3(gl_GlobalInvocationID));

	/* NOTE: used for constant F# dynamic receive apodization. This is implemented as:
	 *
	 *                  /        |x_e - x_i|\
	 *    a(x, z) = cos(F# * π * ----------- ) ^ 2
	 *                  \        |z_e - z_i|/
	 *
	 * where x,z_e are transducer element positions and x,z_i are image positions. */
	float apod_arg = f_number * radians(180) / abs(image_point.z);

	/* NOTE: skip over channels corresponding to other arrays */
	uint starting_offset = u_xdc_index * (dec_data_dim.y / xdc_count) * dec_data_dim.x * dec_data_dim.z;

	/* NOTE: array edge vectors for calculating element step delta */
	vec3 edge1 = xdc_corner1[u_xdc_index].xyz - xdc_origin[u_xdc_index].xyz;
	vec3 edge2 = xdc_corner2[u_xdc_index].xyz - xdc_origin[u_xdc_index].xyz;
	vec3 delta;

	vec2 sum;
	switch (das_shader_id) {
	case DAS_ID_UFORCES:
		/* TODO: there should be a smarter way of detecting this.
		 * (hack since we assume we beamform in x-z even when we are actually doing
		 *  y-z. this will be cleaned up by using an initial transform to shift the
		 *  voxel into the correct space) */
		float y_off;
		if (edge2.x != 0) {
			delta = vec3(edge2.x, 0, 0) / float(dec_data_dim.y - 1);
			y_off = edge1.y / 2;
		} else {
			delta = vec3(edge1.x, 0, 0) / float(dec_data_dim.y - 1);
			y_off = edge2.y / 2;
		}

		sum = uFORCES(image_point, delta, y_off, starting_offset, apod_arg);
		break;
	case DAS_ID_HERCULES:
		/* TODO: there should be a smarter way of detecting this */
		if (edge2.x != 0) delta = vec3(edge2.x, edge1.y, 0) / float(dec_data_dim.y);
		else              delta = vec3(edge1.x, edge2.y, 0) / float(dec_data_dim.y);
		sum = HERCULES(image_point, delta, starting_offset, apod_arg);
		break;
	case DAS_ID_RCA:
		/* TODO: there should be a smarter way of detecting this */
		if (edge2.x != 0) delta = vec3(edge2.x, edge1.y, 0) / float(dec_data_dim.y);
		else              delta = vec3(edge1.x, edge2.y, 0) / float(dec_data_dim.y);
		sum = RCA(image_point, delta, starting_offset, apod_arg);
		break;
	}

	imageStore(u_out_data_tex, out_coord, vec4(sum, 0, 0));
}
