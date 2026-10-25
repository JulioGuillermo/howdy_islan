const SUBSTEP_SECONDS = 1 / 480;
const MAX_STEP_SECONDS = 0.05;
const SETTLED_DISTANCE = 0.2;

/* A value that moves toward its target with a bounce. */
export class Spring {
    constructor(value, stiffness, damping) {
        this.value = value;
        this.target = value;
        this.velocity = 0;
        this.stiffness = stiffness;
        this.damping = damping;
    }

    step(seconds) {
        if (this.isSettled()) {
            this.value = this.target;
            this.velocity = 0;
            return;
        }

        let remaining = Math.min(seconds, MAX_STEP_SECONDS);
        while (remaining > 0) {
            const step = Math.min(remaining, SUBSTEP_SECONDS);
            const force = this.stiffness * (this.target - this.value) - this.damping * this.velocity;
            this.velocity += force * step;
            this.value += this.velocity * step;
            remaining -= step;
        }
    }

    isSettled() {
        return Math.abs(this.target - this.value) < SETTLED_DISTANCE && Math.abs(this.velocity) < SETTLED_DISTANCE;
    }
}
