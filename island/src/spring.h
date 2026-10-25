#pragma once

#include <stdbool.h>

/* A value that moves toward its target with a bounce. */
struct spring {
	double value;
	double target;
	double velocity;
	double stiffness;
	double damping;
};

void spring_init(struct spring *spring, double value, double stiffness, double damping);
void spring_step(struct spring *spring, double seconds);
bool spring_is_settled(const struct spring *spring);
