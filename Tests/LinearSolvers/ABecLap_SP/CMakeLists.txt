if (AMReX_SPACEDIM EQUAL 1)
   return()
endif ()

set(_sources
   main.cpp
   MyTest.cpp
   initProb.cpp
   MyTestPlotfile.cpp
   MyTest.H
   initProb_K.H)

set(_input_files inputs)

setup_test(_sources _input_files)

unset(_sources)
unset(_input_files)
