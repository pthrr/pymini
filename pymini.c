/*****************************************************************************\

  pymini.c - embedded python 3.5 launcher for pymini.py

  (c) 2013-2017 Copyright Eckler Software

  Author: David Suffield, dsuffiel@ecklersoft.com

  This program is free software; you can redistribute it and/or modify
  it under the terms of version 2 of the GNU General Public License as published by
  the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA

  Upstream patches are welcome. Any patches submitted to the author must be 
  unencumbered (ie: no Copyright or License). Patches that are accepted will 
  be applied to the GPL version, but the author reserves the rights to 
  Copyright and License.  

\*****************************************************************************/
#include <Python.h>

int main(int argc, char** argv)
{
    FILE *fp;
    wchar_t *program = NULL;
    wchar_t **wargv = NULL;
    int i;

    program = Py_DecodeLocale(argv[0], NULL);
    Py_SetProgramName(program);

    Py_Initialize();

    wargv = (wchar_t**) malloc(argc * sizeof(wchar_t*));
    for (i = 0; i < argc; i++)
        wargv[i] = Py_DecodeLocale(argv[i], NULL);
    PySys_SetArgv(argc, wargv);

    fp = _Py_fopen("pymini.py", "r");
    PyRun_SimpleFile(fp, "pymini.py");

    Py_Finalize();
    PyMem_RawFree(program);
    for (i = 0; i < argc; i++)
        PyMem_RawFree(wargv[i]);
    free(wargv);
    fclose(fp);
    return 0;
}

