# CONTRIBUTING

## Bug Reports

Did you encounter an issue with using Slic3r? Fear not! This guide will help you to write a good bug report in just a few, simple steps.

There is a good chance that the issue, you have encountered, is already reported. Please check the [list of reported issues](https://github.com/alexrj/Slic3r/issues) before creating a new issue report. If you find an existing issue report, feel free to add further information to that report.

If you are reporting an issue relating to a release version of Slic3r, it would help a lot if you could also confirm that the behavior is still present in the newest build [(windows)](https://bintray.com/lordofhyphens/Slic3r/slic3r_dev/). Otherwise your issue will be closed as soon as someone else isn't able to reproduce it on current master.

When possible, please include the following information when [reporting an issue](https://github.com/alexrj/Slic3r/issues/new):
* Slic3r version (See the about dialog for the version number. If running from git, please include the git commit ID from `git rev-parse HEAD` also.)
* Operating system type + version
* Steps to reproduce the issue, including:
    * Command line parameters used, if any
    * Slic3r configuration file (Use ``Export Config...`` from the ``File`` menu - please don't export a bundle)
    * Expected result
    * Actual result
    * Any error messages
* If the issue is related to G-code generation, please include the following:
    * STL, OBJ or AMF input file (please make sure the input file is not broken, e.g. non-manifold, before reporting a bug)
    * a screenshot of the G-code layer with the issue (e.g. using [Pronterface](https://github.com/kliment/Printrun) or preferably the internal preview tab in Slic3r).
* If the issue is a request for a new feature, be ready to explain why you think it's needed.
    * Doing more preparatory work on your end makes it more likely it'll get done. This includes the "how" it can be done in addition to the "what". 
    * Define the "What" as strictly as you can. Consider what might happen with different infills than simple rectilinear.

Please make sure only to include one issue per report. If you encounter multiple, unrelated issues, please report them as such.

Simon Tatham has written an excellent on article on [How to Report Bugs Effectively](http://www.chiark.greenend.org.uk/~sgtatham/bugs.html) which is well worth reading, although it is not specific to Slic3r.

## Submitting a Patch

Do you want to help fix issues in or add features to Slic3r? That's also very, very welcome :)

Basically you can follow the [GitHub
flow](https://guides.github.com/introduction/flow/) for submitting
patches, but please also read below for more specific information about
contributing to the Slic3r project.

* Development
	* [Low Effort tasks](https://github.com/slic3r/Slic3r/labels/Low%20Effort): pick one of them!
	* [Help Wanted tasks](https://github.com/slic3r/Slic3r/labels/help%20wanted): pick one of them!
	* [More available tasks](https://github.com/slic3r/Slic3r/milestone/32): let's discuss together before you start working on them
	* Please comment in the related GitHub issue that you are working on it so that other people know.
* Review the [TODO wiki page](https://github.com/slic3r/Slic3r/wiki/TODO).
* Contribute to the [Manual](http://manual.slic3r.org/)! (see its [GitHub repository](https://github.com/slic3r/Slic3r-Manual))
* Add an [issue](https://github.com/slic3r/Slic3r/issues) to the GitHub tracker if it isn't already present.
* Update Slic3r's test suite to improve test coverage [Writing Test Cases](https://github.com/slic3r/Slic3r/wiki/Code:-Writing-Test-Cases)
* A good place to start if you can is to look over the [Pull Request or Bust](https://github.com/alexrj/Slic3r/milestones/Pull%20Request%20or%20Bust) milestone. This contains all of the things (mostly new feature requests) that there isn't time or resources to address at this time. 
     * Things that are probably fixable via scripts (usually marked as such) have the lowest bar to getting something that works, as you don't need to recompile Slic3r to test.
* If you're starting on an issue, please say something in the related issues thread so that someone else doesn't start working on it too.
* If there's nothing in the [Pull Request or Bust](https://github.com/alexrj/Slic3r/milestones/Pull%20Request%20or%20Bust) milestone that interests you, the next place to look is for issues that don't have a milestone. Lots of people commit ideas to Slic3r, and it's difficult to keep up and sort through them.
* Before sending a pull request, please make sure that the changes you are submitting are contained in their own git branch, as PRs merge histories.
     * Pull requests that contain unrelated changes will be rejected.
     * A common workflow is to fork the master branch, create your new branch and do your work there. git-rebase and git-cherry-pick are also helpful for separating out unrelated changes.
* If you are pushing Slic3r code changes that touch the main application, it is very much appreciated if you write some tests that check the functionality of the code. It's much easier to vet and merge in code that includes tests and doing so will likely speed things up.

## Communication

* #slic3r on [Libera.Chat](https://libera.chat/): talk to _Sound_, _LoH_ or the other members of the Slic3r community.
* Drop Alessandro a line at aar@cpan.org.
* Drop Joseph a line at lenox.joseph@gmail.com
