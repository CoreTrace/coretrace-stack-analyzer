// SPDX-License-Identifier: Apache-2.0
enum class DiagnosticSeverity
{
    Info = 0,
    Warning = 1,
    Error = 2
};

int main(int argc, char** argv)
{
    int iter = 13;
    DiagnosticSeverity severity;

    if (argc == 2)
        severity = DiagnosticSeverity::Warning;
    if (argc == 3)
        severity = DiagnosticSeverity::Error;
    else
        severity = DiagnosticSeverity::Info;

    for (int i = 0; i < iter; ++i)
    {
        if (severity != DiagnosticSeverity::Info)
        {
            return 1;
        }
        // at line 29, column 27
        // [ !!Warn ] unreachable else-if branch: condition is equivalent to a previous 'if' condition
        //          ↳ else branch implies previous condition is false
        else if (severity != DiagnosticSeverity::Info)
        {
            return 1;
        }
    }
    return 0;
}
