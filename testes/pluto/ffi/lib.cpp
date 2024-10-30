#include "../../../src/vendor/Soup/soup/base.hpp"

SOUP_CEXPORT int MY_MAGIC_INT = 69;

SOUP_CEXPORT int add(int a, int b)
{
	return a + b;
}

struct Result
{
    int sum;
    int product;
};

SOUP_CEXPORT void quick_maffs(Result *out, int a, int b)
{
    out->sum = a + b;
    out->product = a * b;
}
