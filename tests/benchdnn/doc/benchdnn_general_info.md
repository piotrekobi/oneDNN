# General Notes and Details

## Return status

Returns `1` if any submitted tests returned status `FAILED` or `UNIMPLEMENTED`,
`0` otherwise.

## Running Tests

oneDNN comes with its own testing infrastructure enabled through CMake. Tests
can be executed via the command:
``` sh
    make test_<test-name>
```
This instructs CMake to build a deployable project and run the specific test.

These tests target specific oneDNN features and are based on benchdnn
configurable executions.

The available tests can be found in the oneDNN directory:
tests/benchdnn/inputs/<primitive_name>/test_<test-name>.

## Glossary

| Abbreviation | Description
| :---         | :---
| src          | Source/input image
| wei          | Weights (or filter)
| bia          | Bias
| dst          | Destination/output image
| acc          | Accumulation (typically in terms of data type)

## Modes

**benchdnn** supports several execution flows ("modes"). Driver takes the
following steps to execute any flow:
1. Parse user input.
2. Iterate over multiple selected options for each problem descriptor and create
   a driver problem object for each setup. Each setup continues doing next
   steps.
3. Call backend API to create backend objects to execute.
4. Create memory objects for backend and reference paths and fill them with
   reasonable data.
5. Execute backend path.
6. Correctness validation:
   * Check that padded area, if present, is properly zeroed for each memory.
   * For GPU: check that the backend didn't write out-of-boundary.
   * Execute reference path.
   * Setup compare object.
   * Compare outputs of backend and reference and save the status.
7. Performance validation:
   * Execute backend path in a loop until one of selected criterion to stop is
     triggered. Refer to [performance options](knobs_common.md) for details.
8. Report a test case status and repro line.
   * If performance validation was requested, print a performance report output
     based on selected options and collected statistics.
9. Repeat steps 2-7 until all setups are validated.
10. Report the summary and return the status.

The following modes are supported:
* Correctness mode: This is the default driver flow. It executes steps above
  skipping step 7.
* Performance mode: This flow executes steps above skipping step 6.
* Correctness & performance mode: This flow executes all step above.
* Run mode: This flow executes steps 1-5 above. It allows to save time from
  running correctness when it is not needed. This mode is compatible with
  correctness or performance mode, though it will no longer be a run mode, but
  correctness or performance one.
* Listing mode: This flow executes steps 1-2 above. It allows to validate input
  files by parsing syntax and check if all problem repro lines are expected.
  This mode is standalone and is not compatible with other modes.

## Problem Statuses

Each problem in **benchdnn** receives a status reflecting the outcome of the
problem execution. Following statuses are supported (in order of processing the
problem):
* `LISTED`. It means that a driver problem object was created, and the
  reproducer line might be reported. The execution was stopped before creating
  any library objects.
* `SKIPPED`. Same as `LISTED` but the execution was stopped intentionally for
  the reason given in the short description, e.g. `CASE_NOT_SUPPORTED` or
  `SKIP_IMPL_HIT`.
  Note: Nvidia backend is treated specially. See a note below.
* `INVALID_ARGUMENTS`. It means that the library API returned an error due to
  incorrect argument values. It is treated as a failure.
* `UNIMPLEMENTED`. It means that the library does not have an implementation for
  a requested problem. It is treated as a failure.
  Note: All Nvidia backend `unimplemented` status errors are always treated as
  `SKIPPED (CASE_NOT_SUPPORTED)` to simplify validation.
* `EXECUTED`. It means that a problem was run, and the library execution call
  was successful, but the correctness was not validated.
* `PASSED`. It means that a problem passed the correctness validation, and the
  library output matches the driver's reference output.
* `MISTRUSTED`. It means that the quality of correctness validation is under
  question. This often happens when the ratio of the number of zero values to
  the total number of elements in the output exceeds a certain threshold. One
  possible reason is improper filling of input data for a given driver,
  specific algorithm, or a specific problem. Though the validation may not
  fulfil the purpose, as long as values are same for driver reference and the
  library outputs, it is not treated as a failure.
* `FAILED`. It means that a problem did not pass the correctness validation,
  and the library output differs from the driver's reference output.
* `UNTESTED`. It means that none of above statuses were assigned, and the
  execution was aborted at unexpected place. It is treated as a failure.

## Input Files Naming Convention

Benchdnn follows certain [guidelines](benchdnn_input_files_naming_convention.md)
regarding input files naming convention.
