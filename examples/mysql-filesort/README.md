# Distributed MySQL FileSort

When MySQL runs out of memory, it uses filesort. The general approach is to first read the data into the sort buffer. If the buffer is insufficient, it attempts to expand the buffer. If it cannot allocate new space, it saves the data in chunks to a temporary file named tempfile. When space is insufficient, it first performs quicksort in the sort buffer, then writes the sorted data to disk. After all the data has been processed, it uses merge sort to finally obtain a complete ordered dataset.  

In this example, We focus on the quicksort part of the MySQL filesort process. We use the MatrixCPP API and a shared file system to send the calculation of the quicksort part to other nodes for execution, thus achieving distributed execution.

# How to use

1. Required before building: Install Ray using the recommended command:  
  pip install -U ray[cpp]==2.48.0

2. Building method:  
  Add the -M option when building the LLVM command.

3. To build the MatrixCPP library from source:  
  mkdir build && cd build  
  cmake -DRAY_CPP_PATH=path/to/ray ..  
  make -j

4. Apply Patch  
  MySQL version : 8.0.27  
  commit : 3290a66c89eb1625a7058e0ef732432b6952b435  
  git apply support-remote-sort.patch

5. Add dependency of MatrixCPP when compiling MySQL:  
  cmake .. ... -DMATRIXCPP_SOURCE_PATH=/path/to/MatrixCPP ...

6. export LD_LIBRARY_PATH=\$RAY_CPP_PATH/lib:\$LD_LIBRARY_PATH

7. Start ray cluster  
  ray start --head # master node  
  ray start --address='ip of master' # slave node

8. Run mysql  
  mysqld ... --tmpdir=/nfs_storage

# Evaluation

1. prepare data
```
create table t1(a int, b int, c datetime);

delimiter $$
create procedure insert_data(in counts int)
begin
  declare v1 int;
  set v1 = 0;
  set autocommit = 0;
  while v1 < counts do
    insert into t1 values (v1, floor(rand()*100) % 100, now());
    set v1 = v1 + 1;
  end while;
  commit;
end$$;

delimiter ;
call insert_data(100000000);
```
2. set sort_buffer_size = 1024 * 1024 * 256;
3. set session use_remote_sort = on;
4. select * from t1 order by a desc;

Under the same parameters, we run the cluster on two nodes and the execution time has been optimized from 156 seconds to 145 seconds compare to on a single machine. The entrie sort process can be broken down as shown in the table below. Since we only modified the quick-sort part, the execution time for the other parts remains almost unchanged.
|  | local | distributed |
|---|---|---|
|read from disk | 89.56s | 89.82s |
|quick sort | 34.01s | 24.34s |
|wrtie to tempfile | 4.39s | 4.37s |
|merge sort | 5.78s | 5.41s |
|return data | 21.65s | 21.62s |
|total | 156.73s | 145.74s |