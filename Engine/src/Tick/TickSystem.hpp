#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>

namespace Tick
{
    class FixedTickAccumulator
    {
    public:
        explicit FixedTickAccumulator(float fixed_delta = 1.0f / 60.0f, uint32_t max_steps_per_advance = 8)
            : m_fixed_delta(sanitizeFixedDelta(fixed_delta))
            , m_max_steps_per_advance(std::max(1u, max_steps_per_advance))
        {
        }

        void setFixedDelta(float fixed_delta)
        {
            m_fixed_delta = sanitizeFixedDelta(fixed_delta);
            clampAccumulator();
        }

        float getFixedDelta() const { return m_fixed_delta; }

        void setMaxStepsPerAdvance(uint32_t max_steps)
        {
            m_max_steps_per_advance = std::max(1u, max_steps);
            clampAccumulator();
        }

        uint32_t getMaxStepsPerAdvance() const { return m_max_steps_per_advance; }

        uint32_t consume(float delta_time)
        {
            if (!std::isfinite(delta_time) || delta_time <= 0.0f)
                return 0;

            m_accumulator += delta_time;
            clampAccumulator();

            const float epsilon = m_fixed_delta * 1.0e-4f;
            uint32_t steps = 0;
            while (m_accumulator + epsilon >= m_fixed_delta && steps < m_max_steps_per_advance)
            {
                m_accumulator -= m_fixed_delta;
                if (m_accumulator < epsilon)
                    m_accumulator = 0.0f;
                ++steps;
            }
            return steps;
        }

        float getAccumulatedTime() const { return m_accumulator; }

        float getAlpha() const
        {
            return m_fixed_delta > 0.0f ? m_accumulator / m_fixed_delta : 0.0f;
        }

        void reset(float accumulated_time = 0.0f)
        {
            m_accumulator = std::isfinite(accumulated_time) ? std::max(accumulated_time, 0.0f) : 0.0f;
            clampAccumulator();
        }

    private:
        static float sanitizeFixedDelta(float fixed_delta)
        {
            return (std::isfinite(fixed_delta) && fixed_delta > 0.0f) ? fixed_delta : 0.0001f;
        }

        void clampAccumulator()
        {
            const float max_accumulation = m_fixed_delta * static_cast<float>(m_max_steps_per_advance);
            if (m_accumulator > max_accumulation)
                m_accumulator = max_accumulation;
        }

        float m_fixed_delta = 1.0f / 60.0f;
        float m_accumulator = 0.0f;
        uint32_t m_max_steps_per_advance = 8;
    };
}
