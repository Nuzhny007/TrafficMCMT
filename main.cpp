#include <iostream>
#include <chrono>

#include <opencv2/opencv.hpp>
#include <opencv2/core/ocl.hpp>

#include <mdetection/BaseDetector.h>
#include <mtracking/BaseTracker.h>


///----------------------------------------------------------------------
int main(int argc, char** argv)
{
    const char* keys =
    {
        "{ @1                  |../data/atrium.avi  | movie file | }"
        "{ out                 |                    | Name of result video file | }"
        "{ g gpu               |0                   | Use OpenCL acceleration | }"
    };

    cv::CommandLineParser parser(argc, argv, keys);

    parser.printMessage();

    return 0;
}
