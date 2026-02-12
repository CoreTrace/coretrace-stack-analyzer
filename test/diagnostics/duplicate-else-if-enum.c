typedef enum DiagnosticSeverity
{
    Info = 0,
    Warning = 1,
    Error = 2
} DiagnosticSeverity;

int main(int argc, char** argv)
{
    int iter = 13;
    DiagnosticSeverity severity;

    if (argc == 2)
        severity = Warning;
    if (argc == 3)
        severity = Error;
    else
        severity = Info;

    for (int i = 0; i < iter; ++i)
    {
        if (severity != Info)
        {
            return 1;
        }
        // at line 29, column 27
        // [ !!Warn ] unreachable else-if branch: condition is equivalent to a previous 'if' condition
        //          ↳ else branch implies previous condition is false
        else if (severity != Info)
        {
            return 1;
        }
    }
    return 0;
}
