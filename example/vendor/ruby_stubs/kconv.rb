# Stub for Ruby 3.4+, which removed 'kconv' from the standard library with no
# replacement gem published to rubygems.org.
#
# CocoaPods' `xcodeproj` depends on `CFPropertyList` (every version compatible
# with the `xcodeproj`/`cocoapods` versions this Gemfile can resolve to
# requires `CFPropertyList < 4.0`), whose `lib/cfpropertylist/rbCFPropertyList.rb`
# has a single top-level `require 'kconv'` — and never references the `Kconv`
# module anywhere else in the gem. On Ruby 3.4 that bare `require` raises
# `LoadError: cannot load such file -- kconv` the first time CocoaPods reads
# ANY plist (i.e. immediately, on every real `pod install`), even though
# nothing downstream actually needs it.
#
# This file exists purely so `require 'kconv'` succeeds; see
# `example/Gemfile`, which adds this directory to `$LOAD_PATH`. It
# deliberately defines nothing — CFPropertyList never calls into `Kconv`.
