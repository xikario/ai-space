# Offline extraction dependencies

Pure Python dependencies shipped beside the extraction worker so the desktop's
system Python and shell Python have the same XLS/OLE support:

- xlrd 2.0.2: https://pypi.org/project/xlrd/2.0.2/ (BSD license)
- olefile 0.47: https://pypi.org/project/olefile/0.47/ (BSD license)

Original licenses and distribution metadata are retained in the package and
`.dist-info` directories. No office process or network service is started by
these dependencies. Newer Office containers use the Python standard library.
