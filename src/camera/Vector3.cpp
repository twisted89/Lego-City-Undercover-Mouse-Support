#include "Vector3.h"

Vector3 Vector3::operator-(const Vector3& vec) {
	return Vector3(x - vec.x, y - vec.y, z - vec.z);
}

Vector3 Vector3::operator+(const Vector3& vec) {
	return Vector3(x + vec.x, y + vec.y, z + vec.z);
}

float Vector3::Dot(const Vector3& vec) {
	return x * vec.x + y * vec.y + z * vec.z;
}

float Vector3::Dist(const Vector3& vec) {
	return fabs(sqrt(pow(this->x - vec.x, 2) + pow(this->y - vec.y, 2) + pow(this->z - vec.z, 2)));
}