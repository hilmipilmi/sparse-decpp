use Data::Dumper;
use Getopt::Long;
use Getopt::Long;
use Carp;
use FindBin qw($Bin);
use lib "$Bin/../lib";
use Cwd;
use Cwd 'abs_path';

$idre = $id = qr'(?:[a-zA-Z_][a-zA-Z_0-9:\.]*)';
$RE_balanced_squarebrackets =    qr'(?:[\[]((?:(?>[^\[\]]+)|(??{$RE_balanced_squarebrackets}))*)[\]])';
$RE_balanced_smothbrackets =     qr'(?:[\(]((?:(?>[^\(\)]+)|(??{$RE_balanced_smothbrackets}))*)[\)])';
$RE_balanced_brackets      =     qr'(?:[\{]((?:(?>[^\{\}]+)|(??{$RE_balanced_brackets}))*)[\}])';
$RE_comment_Cpp =                q{(?:\/\*(?:(?!\*\/)[\s\S])*\*\/|\/\/[^\n]*\n)};

Getopt::Long::Configure(qw(bundling));
GetOptions(\%OPT,qw{
d+
quite|q+
verbose|v+
outfile|o=s
}, @g_more) or usave(\*STDERR);

sub readfile {
    my ($in) = @_;
    usage(\*STDOUT) if (length($in) == 0) ;
    open IN, "$in" or die "Reading \"$in\":".$!;
    local $/ = undef;
    $m = <IN>;
    close IN;
    return $m;
}

$m = readfile ($ARGV[0]);

sub delspace { my ($m) = @_; $m =~ s/^\s+//s; $m; }
sub rmspace  { my ($m) = @_; $m =~ s/^\s+//s; $m =~ s/\s+$//s; $m; }
sub nrmspace { my ($m) = @_; $m =~ s/\s+/ /s; rmspace($m); }
sub unspace  { my ($m) = @_; $m =~ s/\s+/_/s; $m; }
sub dbgstr   { my ($m,$l) = @_; $m =~ s/\n/\\n/g; return substr($m, 0, $l).(length($m)>$l?"...":""); }
sub ident    { my ($ctx) = @_; my $r = ""; for (my $i = 0; $i < $$ctx{'i'}; $i++) { $r .= "|"; }; return $r; }

while ($m =~ /($idre)$RE_balanced_smothbrackets:\s*\n/m) {
  my ($id, $typ) = ($1,$2);
  $m = $';
#  print (STDERR "$id,$typ\n");
  my @m = ();
  while (($m =~ /^((?:[ \t]+[^\n]*\n))/s)) {
    $m = $'; my $l = $1;
    #print ".".$l.":";
    if ($l =~ /(.+)\s+:\s+($idre)(.*)/) {
      my ($t,$n,$r) = ($1,$2,$3);
      my $p = $n,;
      $p =~ s/\./_/g;
      my $a = {};
      if ($r =~ /$RE_balanced_brackets/) {
	eval ("\$a = { $1 };");
      }
      push(@m,{'n'=>nrmspace($n),'t'=>nrmspace($t),'a'=>$a});
      my $vpost = $$a{'vpost'};
      my $name = $$a{'n'} ? $$a{'n'} : $p;

my $g = "
MODULE = d   PACKAGE = ${id}Ptr
PROTOTYPES: ENABLE

".nrmspace($t)." $vpost
get_${name}(p)
        $id *p
    PREINIT:
    CODE:
        RETVAL = p->$n;
    OUTPUT:
	RETVAL
";
      print $g;

      my $cast = $$a{'cast'};

my $s = "
void
set_${name}(p,v)
        $id *p
        ".nrmspace($t)." $vpost v
    PREINIT:
    CODE:
        p->$n = $cast v;
";
      print $s;


    }
  }
  
}
