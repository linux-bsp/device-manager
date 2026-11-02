BEGIN { print "static const unsigned char admin_page[] = {" }
{ for (i = 1; i <= NF; i++) printf "%s,", $i; print "" }
END { print "0};" }
