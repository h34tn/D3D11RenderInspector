#include <cstdint>
#include <cstdio>
extern "C" uint64_t Forward0(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" double Forward1(double, double, double, double, double, double);
static uint64_t Integer(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h)
{
    return a + 3 * b + 5 * c + 7 * d + 11 * e + 13 * f + 17 * g + 19 * h;
}
static double Floating(double a, double b, double c, double d, double e, double f)
{
    return a + 3 * b + 5 * c + 7 * d + 11 * e + 13 * f;
}
extern "C" void *ResolveRequired(unsigned index)
{
    volatile double scratch[4] = {1.1, 2.2, 3.3, 4.4};
    scratch[0] = scratch[1] * scratch[2] + scratch[3];
    return index == 0 ? (void *)&Integer : (void *)&Floating;
}
int main()
{
    auto a = Forward0(11, 22, 33, 44, 55, 66, 77, 88);
    auto b = Forward1(1.25, 2.5, 3.75, 5, 6.25, 7.5);
    if (a != Integer(11, 22, 33, 44, 55, 66, 77, 88) || b != Floating(1.25, 2.5, 3.75, 5, 6.25, 7.5))
    {
        puts("FAIL forwarding ABI");
        return 1;
    }
    puts("PASS integer, floating-point and stack forwarding ABI");
    return 0;
}
