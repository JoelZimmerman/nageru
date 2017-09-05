#! /usr/bin/perl

#
# A script to measure the quality and speed of the x264 presets used in speed control.
#

use strict;
use warnings;
use Time::HiRes;

my $ssim_mode = 1;
my $output_cpp = 1;
my $flags = "--bitrate 4000 --frames 1000";
my $override_flags = "--weightp 1 --mbtree --rc-lookahead 20";
my $file = "elephants_dream_1080p24.y4m";  # https://media.xiph.org/video/derf/y4m/elephants_dream_1080p24.y4m

if ($ssim_mode) {
	# This can be run on a faster machine if you want to. It just measures SSIM;
	# don't trust the timings, not even which modes are faster than others.
	# The mode where $output_cpp=0 is just meant as a quick way to test new presets
	# to see if they are good candidates.
	$flags .= " --threads 40 --ssim";
	$override_flags .= " --tune ssim";
	open my $fh, "<", "presets.txt"
		or die "presets.txt: $!";
	my $preset_num = 0;
	for my $preset (<$fh>) {
		chomp $preset;
		my ($ssim, $elapsed) = measure_preset($file, $flags, $override_flags, $preset);
		if ($output_cpp) {
			output_cpp($file, $flags, $override_flags, $preset, $ssim, $preset_num++);
		} else {
			printf "%sdb %.3f %s\n", $ssim, $elapsed, $preset;
		}
	}
	close $fh;
} else {
	# Actual benchmarking.
	my $repeat = 1;
	$flags .= " --threads 4";
	open my $fh, "<", "presets.txt"
		or die "presets.txt: $!";
	my $base = undef;
	for my $preset (<$fh>) {
		chomp $preset;
		my $sum_elapsed = 0.0;
		for my $i (1..$repeat) {
			my (undef, $elapsed) = measure_preset($file, $flags, $override_flags, $preset);
			$sum_elapsed += $elapsed;
		}
		my $avg = $sum_elapsed / $repeat;
		$base //= $avg;
		printf "%.3f %s\n", $avg / $base, $preset;
	}
	close $fh;
}

sub measure_preset {
	my ($file, $flags, $override_flags, $preset) = @_;

	my $now = [Time::HiRes::gettimeofday];
	my $ssim;
	open my $x264, "-|", "/usr/bin/x264 $flags $preset $override_flags -o /dev/null $file 2>&1";
	for my $line (<$x264>) {
		$line =~ /SSIM Mean.*\((\d+\.\d+)db\)/ and $ssim = $1;
	}
	close $x264;
	my $elapsed = Time::HiRes::tv_interval($now);
	return ($ssim, $elapsed);
}

sub output_cpp {
	my ($file, $flags, $override_flags, $preset, $ssim, $preset_num) = @_;
	unlink("tmp.h264");
	system("/usr/bin/x264 $flags $preset $override_flags --frames 1 -o tmp.h264 $file >/dev/null 2>&1");
	open my $fh, "<", "tmp.h264"
		or die "tmp.h264: $!";
	my $raw;
	{
		local $/ = undef;
		$raw = <$fh>;
	}
	close $fh;

	$raw =~ /subme=(\d+)/ or die;
	my $subme = $1;

	$raw =~ /me=(\S+)/ or die;
	my $me = "X264_ME_" . uc($1);

	$raw =~ /ref=(\d+)/ or die;
	my $refs = $1;

	$raw =~ /mixed_ref=(\d+)/ or die;
	my $mix = $1;

	$raw =~ /trellis=(\d+)/ or die;
	my $trellis = $1;

	$raw =~ /analyse=0x[0-9a-f]+:(0x[0-9a-f]+)/ or die;
	my $partitions_hex = oct($1);
	my @partitions = ();
	push @partitions, 'I8' if ($partitions_hex & 0x0002);
	push @partitions, 'I4' if ($partitions_hex & 0x0001);
	push @partitions, 'P8' if ($partitions_hex & 0x0010);
	push @partitions, 'B8' if ($partitions_hex & 0x0100);
	push @partitions, 'P4' if ($partitions_hex & 0x0020);
	my $partitions = join('|', @partitions);

	$raw =~ /bframes=(\d+)/ or die;
	my $bframes = $1;

	my ($badapt, $direct);
	if ($bframes > 0) {
		$raw =~ /b_adapt=(\d+)/ or die;
		$badapt = $1;
		$raw =~ /direct=(\d+)/ or die;
		$direct = $1;
	} else {
		$badapt = $direct = 0;
	}

	$raw =~ /me_range=(\d+)/ or die;
	my $merange = $1;

	print "\n";
	print "\t// Preset $preset_num: ${ssim}db, $preset\n";
	print "\t{ .time= 0.000, .subme=$subme, .me=$me, .refs=$refs, .mix=$mix, .trellis=$trellis, .partitions=$partitions, .badapt=$badapt, .bframes=$bframes, .direct=$direct, .merange=$merange },\n";

#x264 - core 148 r2705 3f5ed56 - H.264/MPEG-4 AVC codec - Copyleft 2003-2016 - http://www.videolan.org/x264.html - options: cabac=1 ref=3 deblock=1:0:0 analyse=0x3:0x113 me=hex subme=7 psy=1 psy_rd=1.00:0.00 mixed_ref=1 me_range=16 chroma_me=1 trellis=1 8x8dct=1 cqm=0 deadzone=21,11 fast_pskip=1 chroma_qp_offset=-2 threads=34 lookahead_threads=5 sliced_threads=0 nr=0 decimate=1 interlaced=0 bluray_compat=0 constrained_intra=0 bframes=3 b_pyramid=2 b_adapt=1 b_bias=0 direct=1 weightb=1 open_gop=0 weightp=2 keyint=250 keyint_min=24 scenecut=40 intra_refresh=0 rc_lookahead=40 rc=crf mbtree=1 crf=23.0 qcomp=0.60 qpmin=0 qpmax=69 qpstep=4 ip_ratio=1.40 aq=1:1.00
}
