#ifndef HOLYC_LINEAR_ALGEBRA_H_
#define HOLYC_LINEAR_ALGEBRA_H_

/* Mat4/Quat moved to matrix.h/quat.h (see those files' own top comments
 * for the split rationale - independent, swappable pieces rather than
 * one monolithic algebra file). Kept here as a transparent re-export so
 * this file's existing consumers keep working unchanged - the same
 * "still exposes them transparently" pattern base_type.h uses for
 * bit.h's BIT()/FLAG_*() macros. */

#include "math/mathx.h"

#endif /* HOLYC_LINEAR_ALGEBRA_H_ */

#ifdef LINEAR_ALGEBRA_IMPLEMENTATION
#ifndef LINEAR_ALGEBRA_IMPLEMENTATION_
#define LINEAR_ALGEBRA_IMPLEMENTATION_

/* forward LINEAR_ALGEBRA_IMPLEMENTATION into VECTOR_IMPLEMENTATION/
 * MATRIX_IMPLEMENTATION/QUAT_IMPLEMENTATION so a consumer that only
 * ever knew about LINEAR_ALGEBRA_IMPLEMENTATION still gets every
 * function body compiled in, not just declarations - each #include's
 * own guard means this only re-enters that file's IMPLEMENTATION
 * block, it does not reprocess the declarations already pulled in
 * above (same pattern math_type.h uses forwarding into vector.h). */
#ifndef VECTOR_IMPLEMENTATION
#define VECTOR_IMPLEMENTATION
#endif
#ifndef MATRIX_IMPLEMENTATION
#define MATRIX_IMPLEMENTATION
#endif
#ifndef QUAT_IMPLEMENTATION
#define QUAT_IMPLEMENTATION
#endif

#include "math/mathx.h"

#endif /* LINEAR_ALGEBRA_IMPLEMENTATION_ */
#endif /* LINEAR_ALGEBRA_IMPLEMENTATION */
