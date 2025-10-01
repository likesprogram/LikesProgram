#include "../../../include/LikesProgram/math/Math.hpp"

namespace LikesProgram {
	namespace Math {
        Vector4 MakeQuaternion(const Vector3& axis, double theta) {
            Vector3 normAxis = axis.Normalized();
            double half = theta / 2.0;
            double s = std::sin(half);
            double w = std::cos(half);
            return Vector4(normAxis[0] * s, normAxis[1] * s, normAxis[2] * s, w);
        }
	}
}