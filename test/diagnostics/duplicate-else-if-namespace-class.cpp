#include <cstddef>

namespace demo
{
    class ConditionMatcher
    {
      public:
        explicit ConditionMatcher(bool active, std::size_t threshold)
            : active_(active), threshold_(threshold)
        {
        }

        bool isActive() const
        {
            return active_;
        }

        bool reaches(std::size_t value) const
        {
            return value >= threshold_;
        }

      private:
        bool active_;
        std::size_t threshold_;
    };
} // namespace demo

int main(int argc, char** argv)
{
    demo::ConditionMatcher matcher(argc > 1, 3u);
    std::size_t value = (argc > 2) ? 5u : 1u;

    if (matcher.isActive())
    {
        return 1;
    }
    // at line 41, column 22
    // [ !!Warn ] unreachable else-if branch: condition is equivalent to a previous 'if' condition
    //          ↳ else branch implies previous condition is false
    else if (matcher.isActive())
    {
        return 2;
    }

    if (matcher.reaches(value))
    {
        return 3;
    }
    // at line 53, column 22
    // [ !!Warn ] unreachable else-if branch: condition is equivalent to a previous 'if' condition
    //          ↳ else branch implies previous condition is false
    else if (matcher.reaches(value))
    {
        return 4;
    }

    (void)argv;
    return 0;
}
