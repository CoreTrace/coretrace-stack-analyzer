int main(void)
{
    // stack-limit: 30
    // at line 13, column 5
    // [!!!Error] potential stack overflow: exceeds limit of 30 bytes
    //          ↳ locals: 5 variables (total 32 bytes)
    // locals list: a(4), b(4), c(4), d(4), retval(4)
    int a;
    int b;
    int c;
    int d;

    return 0;
}

// at line 8, column 1
// [ !!Warn ] local variable 'a' is never initialized
//          ↳ declared without initializer and no definite write was found in this function

// at line 9, column 1
// [ !!Warn ] local variable 'b' is never initialized
//          ↳ declared without initializer and no definite write was found in this function

// at line 10, column 1
// [ !!Warn ] local variable 'c' is never initialized
//          ↳ declared without initializer and no definite write was found in this function

// at line 11, column 1
// [ !!Warn ] local variable 'd' is never initialized
//          ↳ declared without initializer and no definite write was found in this function
