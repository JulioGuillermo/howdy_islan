#include "spring.h"

#include <math.h>

#define SUBSTEP_SECONDS (1.0 / 480.0)
#define MAX_STEP_SECONDS 0.05
#define SETTLED_DISTANCE 0.2

void spring_init(struct spring *spring, double value, double stiffness, double damping)
{
	*spring = (struct spring){
		.value = value,
		.target = value,
		.stiffness = stiffness,
		.damping = damping,
	};
}

void spring_step(struct spring *spring, double seconds)
{
	if (spring_is_settled(spring)) {
		spring->value = spring->target;
		spring->velocity = 0;
		return;
	}

	seconds = fmin(seconds, MAX_STEP_SECONDS);
	while (seconds > 0) {
		double step = fmin(seconds, SUBSTEP_SECONDS);
		double force = spring->stiffness * (spring->target - spring->value) - spring->damping * spring->velocity;
		spring->velocity += force * step;
		spring->value += spring->velocity * step;
		seconds -= step;
	}
}

bool spring_is_settled(const struct spring *spring)
{
	return fabs(spring->target - spring->value) < SETTLED_DISTANCE && fabs(spring->velocity) < SETTLED_DISTANCE;
}
