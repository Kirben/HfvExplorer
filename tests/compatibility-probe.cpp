// SPDX-License-Identifier: GPL-2.0-or-later
#include "WindowsCompatibilityCases.h"
int main(int argc,char** argv){
 FILE* output=stdout;if(argc==2&&fopen_s(&output,argv[1],"w"))return 2;
 auto version=WindowsCompatibility::Current();WindowsCompatibility::Report(output,version);
 bool policy=VerifyWindowsCompatibilityPolicy();fprintf(output,"Prerequisite policy matrix: %s\n",policy?"PASS":"FAIL");
 if(output!=stdout)fclose(output);return WindowsCompatibility::Supported(version)&&policy?0:1;
}
