# Lists the partitions of the WARP table test.t1 and their number of rows.
perl;
  my $dir = "$ENV{MYSQLTEST_VARDIR}/mysqld.1/data/test/t1.data";
  opendir(my $dh, $dir) or die "can not open $dir";
  my @parts = sort { substr($a, 1) <=> substr($b, 1) } grep { /^p\d+$/ } readdir($dh);
  closedir($dh);
  foreach my $p (@parts) {
    open(my $fh, "<", "$dir/$p/-part.txt") or next;
    while (<$fh>) { print "$p $1\n" if /^Number_of_rows\s*=\s*(\d+)/; }
    close($fh);
  }
EOF
