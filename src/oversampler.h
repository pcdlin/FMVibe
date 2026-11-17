#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace fmvibe {

// The coefficients of a half-band's first-order allpasses, `count` of them, for a transition band `transition` wide
// as a fraction of the rate (the elliptic design, after Laurent de Soras' HIIR), from the smallest up: they alternate
// between the two chains. The same pair of chains, run on every other sample, keeps two signals 90 degrees apart
// from `transition` of the rate up to as far short of half of it (see fx::Hilbert).
inline void halfBandCoefs(int count, double transition, double *coefs) {
    // Elliptic modulus and nome of the transition band.
    double k = std::tan((1.0 - 2.0 * transition) * std::numbers::pi / 4.0);
    k *= k;
    const double kk = std::pow(1.0 - k * k, 0.25);
    const double e = 0.5 * (1.0 - kk) / (1.0 + kk);
    const double e4 = e * e * e * e;
    const double q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));
    const int order = 2 * count + 1;
    for (int i = 0; i < count; ++i) {
        const int c = i + 1;
        double num = 0.0, den = 0.0;
        for (int n = 0;; ++n) {
            const double term = std::pow(q, n * (n + 1)) * std::sin((2 * n + 1) * c * std::numbers::pi / order);
            num += n % 2 ? -term : term;
            if (std::abs(term) < 1e-100 || n > 100)
                break;
        }
        for (int n = 1;; ++n) {
            const double term = std::pow(q, n * n) * std::cos(2 * n * c * std::numbers::pi / order);
            den += n % 2 ? -term : term;
            if (std::abs(term) < 1e-100 || n > 100)
                break;
        }
        const double w = num * std::pow(q, 0.25) / (den + 0.5);
        const double w2 = w * w;
        const double x = std::sqrt((1.0 - w2 * k) * (1.0 - w2 / k)) / (1.0 + w2);
        coefs[i] = (1.0 - x) / (1.0 + x);
    }
}

// Half-band low-pass that halves the sample rate: two parallel chains of
// first-order allpasses (polyphase IIR, after Laurent de Soras' HIIR). The
// coefficients come from the elliptic design for a given number of allpasses
// and transition band. Nearly linear phase in the audible band, a few samples
// late (see decimatorDelay() and upDownDelay()).
class HalfBand {
public:
    static constexpr int kMaxCoefs = 12;

    // `transition` is the width of the transition band as a fraction of the
    // input rate: the passband ends at (0.25 - transition) of it.
    void design(int coefCount, double transition) {
        count_ = std::clamp(coefCount, 1, kMaxCoefs);
        halfBandCoefs(count_, transition, coefs_.data());
        reset();
    }

    void reset() {
        x_.fill(0.0);
        y_.fill(0.0);
    }

    // One input sample in, two out: up to twice the rate, the same filter run as an interpolator.
    void processUp(double in, double &first, double &second) {
        double a = in, b = in;
        for (int i = 0; i < count_; i += 2) {
            const double ya = (a - y_[i]) * coefs_[i] + x_[i];
            x_[i] = a;
            y_[i] = ya;
            a = ya;
            if (i + 1 < count_) {
                const double yb = (b - y_[i + 1]) * coefs_[i + 1] + x_[i + 1];
                x_[i + 1] = b;
                y_[i + 1] = yb;
                b = yb;
            }
        }
        first = a;
        second = b;
    }

    // Two input samples in, one out.
    double process(double first, double second) {
        double a = second, b = first; // the even coefficients filter one phase, the odd ones the other
        for (int i = 0; i < count_; i += 2) {
            const double ya = (a - y_[i]) * coefs_[i] + x_[i];
            x_[i] = a;
            y_[i] = ya;
            a = ya;
            if (i + 1 < count_) {
                const double yb = (b - y_[i + 1]) * coefs_[i + 1] + x_[i + 1];
                x_[i + 1] = b;
                y_[i + 1] = yb;
                b = yb;
            }
        }
        return 0.5 * (a + b);
    }

private:
    int count_ = 0;
    std::array<double, kMaxCoefs> coefs_{};
    std::array<double, kMaxCoefs> x_{};
    std::array<double, kMaxCoefs> y_{};
};

// Brings one channel down from 2, 4 or 8 times the host rate in half-band
// steps. The last step, down to the host rate, keeps the audible band up to
// 20 kHz; the earlier ones only have to clear what would fold into it, so they
// get by with fewer allpasses.
class Decimator {
public:
    static constexpr int kMaxFactor = 8;

    void prepare(double hostRate, int factor) {
        stages_ = factor >= 8 ? 3 : factor >= 4 ? 2 : factor >= 2 ? 1 : 0;
        constexpr double kPassband = 20000.0;
        static constexpr int kCoefs[] = {12, 6, 4}; // from the step down to the host rate up
        for (int s = 0; s < stages_; ++s) {
            const double inputRate = hostRate * (2 << s);
            const double transition = std::clamp(0.25 - kPassband / inputRate, 0.01, 0.24);
            stage_[s].design(kCoefs[s], transition);
        }
    }

    void reset() {
        for (HalfBand &s : stage_)
            s.reset();
    }

    // `in` holds frames * factor samples and is overwritten on the way; `out` gets `frames`.
    void process(double *in, float *out, int frames) {
        int n = frames << stages_;
#ifdef __EMSCRIPTEN__
        // WebAssembly can't flush denormals to zero as the plugin's process() does, and the allpasses' states decay
        // into them as a voice fades out, at many times the CPU. A signal far below hearing, at the top of the band
        // where the half-bands take it out, keeps them above.
        for (int i = 0; i < n; ++i)
            in[i] += i % 2 == 0 ? 1e-20 : -1e-20;
#endif
        for (int s = stages_ - 1; s >= 0; --s) {
            n /= 2;
            for (int i = 0; i < n; ++i)
                in[i] = stage_[s].process(in[2 * i], in[2 * i + 1]);
        }
        for (int i = 0; i < frames; ++i)
            out[i] += static_cast<float>(in[i]);
    }

private:
    int stages_ = 0;
    std::array<HalfBand, 3> stage_{};
};

// Brings one channel up from the host rate to 2, 4 or 8 times it in half-band steps, the steepest first: the
// Decimator's steps run the other way.
class Upsampler {
public:
    void prepare(double hostRate, int factor) {
        stages_ = factor >= 8 ? 3 : factor >= 4 ? 2 : factor >= 2 ? 1 : 0;
        constexpr double kPassband = 20000.0;
        static constexpr int kCoefs[] = {12, 6, 4}; // from the step up from the host rate on
        for (int s = 0; s < stages_; ++s) {
            const double outputRate = hostRate * (2 << s);
            const double transition = std::clamp(0.25 - kPassband / outputRate, 0.01, 0.24);
            stage_[s].design(kCoefs[s], transition);
        }
    }

    void reset() {
        for (HalfBand &s : stage_)
            s.reset();
    }

    static constexpr int kMaxSamples = 512; // frames * factor at most

    // `out` gets frames * factor samples.
    void process(const float *in, double *out, int frames) {
        double from[kMaxSamples / 2];
        for (int i = 0; i < frames; ++i)
            out[i] = in[i];
        int n = frames;
        for (int s = 0; s < stages_; ++s) {
            std::copy_n(out, n, from);
            for (int i = 0; i < n; ++i)
                stage_[s].processUp(from[i], out[2 * i], out[2 * i + 1]);
            n *= 2;
        }
    }

private:
    int stages_ = 0;
    std::array<HalfBand, 3> stage_{};
};

// One channel up to `factor` times the host rate and straight back down, with nothing between: exactly as late, at
// every frequency, as a path through a stage oversampled that far (the shaper effect's, the filter effect's, an
// amp's), which is what a path beside one is put through so the two stay in step (see Effects::orderGraph()). A factor
// of 1 passes what goes in as it is.
class UpDown {
public:
    void prepare(double hostRate, int factor) {
        factor_ = factor >= 8 ? 8 : factor >= 4 ? 4 : factor >= 2 ? 2 : 1;
        up_.prepare(hostRate, factor_);
        down_.prepare(hostRate, factor_);
    }
    void reset() {
        up_.reset();
        down_.reset();
    }
    int factor() const { return factor_; }

    // In place, `frames` <= Upsampler::kMaxSamples / 8.
    void run(float *x, int frames) {
        if (factor_ == 1)
            return;
        double buf[Upsampler::kMaxSamples];
        up_.process(x, buf, frames);
        std::fill_n(x, frames, 0.0f);
        down_.process(buf, x, frames);
    }

private:
    Upsampler up_;
    Decimator down_;
    int factor_ = 1;
};

// How late, in host samples, low frequencies come out of the voices' decimator from `factor` times the rate, and out of
// an UpDown at `factor`: the centroid of their impulse responses, their group delay at 0 Hz.
inline double decimatorDelay(double hostRate, int factor) {
    if (factor < 2)
        return 0.0;
    Decimator down;
    down.prepare(hostRate, factor);
    constexpr int kFrames = 32, kRounds = 32;
    double moment = 0.0, sum = 0.0;
    for (int round = 0; round < kRounds; ++round) {
        double in[kFrames * Decimator::kMaxFactor]{};
        if (round == 0)
            in[0] = 1.0;
        float out[kFrames]{};
        down.process(in, out, kFrames);
        for (int i = 0; i < kFrames; ++i) {
            moment += static_cast<double>(round * kFrames + i) * out[i];
            sum += out[i];
        }
    }
    return sum != 0.0 ? moment / sum : 0.0;
}

inline double upDownDelay(double hostRate, int factor) {
    if (factor < 2)
        return 0.0;
    UpDown path;
    path.prepare(hostRate, factor);
    constexpr int kFrames = 32, kRounds = 32;
    double moment = 0.0, sum = 0.0;
    for (int round = 0; round < kRounds; ++round) {
        float x[kFrames]{};
        if (round == 0)
            x[0] = 1.0f;
        path.run(x, kFrames);
        for (int i = 0; i < kFrames; ++i) {
            moment += static_cast<double>(round * kFrames + i) * x[i];
            sum += x[i];
        }
    }
    return sum != 0.0 ? moment / sum : 0.0;
}

} // namespace fmvibe
