#ifndef HOLYC_UNITS_H_
#define HOLYC_UNITS_H_

/* Unit conversion - length (mm/cm/m/km) and weight (mg/g/kg/t), each
 * routed through one base unit (meters, grams) so adding a new unit
 * later only needs its own to/from-base factor, not a new conversion
 * for every existing pair. */

#include "types/base.h"

typedef enum { UNITS_MM, UNITS_CM, UNITS_M, UNITS_KM } Units_Length;
typedef enum { UNITS_MG, UNITS_G, UNITS_KG, UNITS_T } Units_Weight;

f64 units_length_to_m(f64 value, Units_Length from);
f64 units_length_from_m(f64 meters, Units_Length to);
f64 units_length_convert(f64 value, Units_Length from, Units_Length to);

f64 units_weight_to_g(f64 value, Units_Weight from);
f64 units_weight_from_g(f64 grams, Units_Weight to);
f64 units_weight_convert(f64 value, Units_Weight from, Units_Weight to);

#endif /* HOLYC_UNITS_H_ */

#ifdef UNITS_IMPLEMENTATION
#ifndef UNITS_IMPLEMENTATION_
#define UNITS_IMPLEMENTATION_

static f64 units_length_factor_(Units_Length u)
{
    switch (u) {
    case UNITS_MM: return 0.001;
    case UNITS_CM: return 0.01;
    case UNITS_M:  return 1.0;
    case UNITS_KM: return 1000.0;
    }
    return 1.0;
}

f64 units_length_to_m(f64 value, Units_Length from) { return value * units_length_factor_(from); }
f64 units_length_from_m(f64 meters, Units_Length to) { return meters / units_length_factor_(to); }

f64 units_length_convert(f64 value, Units_Length from, Units_Length to)
{
    return units_length_from_m(units_length_to_m(value, from), to);
}

static f64 units_weight_factor_(Units_Weight u)
{
    switch (u) {
    case UNITS_MG: return 0.001;
    case UNITS_G:  return 1.0;
    case UNITS_KG: return 1000.0;
    case UNITS_T:  return 1000000.0;
    }
    return 1.0;
}

f64 units_weight_to_g(f64 value, Units_Weight from) { return value * units_weight_factor_(from); }
f64 units_weight_from_g(f64 grams, Units_Weight to) { return grams / units_weight_factor_(to); }

f64 units_weight_convert(f64 value, Units_Weight from, Units_Weight to)
{
    return units_weight_from_g(units_weight_to_g(value, from), to);
}

#endif /* UNITS_IMPLEMENTATION_ */
#endif /* UNITS_IMPLEMENTATION */
