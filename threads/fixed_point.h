#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

struct FIXED17_14{
	int full;				//Allows for one signed bit
};

static const int CONVERT = 16384;

struct FIXED17_14 int_to_fixed17_14(int integer);

struct FIXED17_14 multiply_fixed17_14(struct FIXED17_14 a, struct FIXED17_14 b);

struct FIXED17_14 divide_fixed17_14(struct FIXED17_14 a, struct FIXED17_14 b);




#endif //fixed_point.h