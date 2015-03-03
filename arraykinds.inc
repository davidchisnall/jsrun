#ifndef TYPED_ARRAY_CASE
#  error TYPED_ARRAY_CASE must be defined before including this file
#endif
TYPED_ARRAY_CASE(Int8, int8_t);
TYPED_ARRAY_CASE(UInt8, uint8_t);
TYPED_ARRAY_CASE(Int16, int16_t);
TYPED_ARRAY_CASE(UInt16, uint16_t);
TYPED_ARRAY_CASE(Int32, int32_t);
TYPED_ARRAY_CASE(UInt32, uint32_t);
TYPED_ARRAY_CASE(Float32, float);
TYPED_ARRAY_CASE(Float64, double);
#undef TYPED_ARRAY_CASE
