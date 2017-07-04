---
layout: page
title: Contributing
permalink: /contributing/
---

* toc
{:toc}

## Getting started with the Kythe codebase

[Instructions to build Kythe from source]({{site.baseuri}}/getting-started)

## Code review

All changes to Kythe must go through code review before being submitted, and
each individual or corporate contributor must sign an appropriate [Contributor
License Agreement](https://cla.developers.google.com/about).  Once your CLA is
submitted (or if you already submitted one for another Google project), make a
commit adding yourself to the
[AUTHORS]({{site.data.development.source_browser}}/AUTHORS) and
[CONTRIBUTORS]({{site.data.development.source_browser}}/CONTRIBUTORS)
files. This commit can be part of your first Differential code review.

The Kythe team has chosen to use a [Phabricator](http://phabricator.org/)
instance located at
[{{site.url}}/phabricator]({{site.url}}{{site.data.development.phabricator}})
for code reviews.  This requires some local setup for each developer:

### Installing arcanist

{% highlight bash %}
ARC_PATH=~/apps/arc # path to install arcanist/libphutil

sudo apt-get install php5 php5-curl
mkdir -p "$ARC_PATH"
pushd "$ARC_PATH"
git clone https://github.com/phacility/libphutil.git
git clone https://github.com/phacility/arcanist.git
popd

# add arc to your PATH
echo "export PATH=\"${ARC_PATH}/arcanist/bin:\$PATH\"" >> ~/.bashrc
source ~/.bashrc

cd $KYTHE_DIR  # your kythe repository root
arc install-certificate
{% endhighlight %}

### Using arcanist

{% highlight bash %}
git checkout master
git checkout -b feature-name # OR arc feature feature-name
# do some changes
git add ...                    # add the changes
git commit -m "Commit message" # commit the changes
arc diff                       # send the commit for review
# go through code review in Phabricator UI...
# get change accepted
{% endhighlight %}

You can reply to comments in
[Differential](https://phabricator-dot-kythe-repo.appspot.com/differential/)
inside Phabricator. To submit additional commits to that same review, just
`git checkout` to the same branch as your original arc feature, and then either
`git commit` to make new commits or `git commit --amend` to tack it on to the
last existing commit. Finally, just re-run `arc diff` to automatically send out
another review request.

**After someone has accepted your diff** in Phabriactor (you should see a green
checkbox saying "This revision is now ready to land"). Core contributors with
write access to the Kythe respository run this command from their arc feature
branch to merge the change into master and push it to Github:

{% highlight bash %}
arc land                       # merge the change into master
{% endhighlight %}

Others should request that someone else land their change for them once the
change has been reviewed and accepted (basically, ask a core contributor to
run these commands):

{% highlight bash %}
# Land a reviewed change
arc patch D1234
arc land
{% endhighlight %}

Once landed, it should show up in github
[commit list](https://github.com/google/kythe/commits/master).

### Style formatting

Kythe C++ code follows the Google style guide. You can run `clang-format` to do
this automatically:

{% highlight bash %}
clang-format -i --style=file <filename>
{% endhighlight %}

If you forgot to do this for a commit, you can amend it easily:

{% highlight bash %}
clang-format i --style=file $(git show --pretty="" --name-only <SHA1>)
git commit --amend
{% endhighlight %}

## Contribution Ideas

Along with working off of our [tasks
list]({{site.data.development.phabricator}}/maniphest) (and, in particular, the
[Wishlist]({{site.data.development.phabricator}}/maniphest/query/uFWarCNL9v7z/)),
there are many ways to contribute to Kythe.

### New Extractors and Indexers

Kythe is built on the idea of having a common set of tools across programming
languages so Kythe is always happy to
[add another language to its family]({{site.baseurl}}/docs/kythe-compatible-compilers.html).

### Build System Integration

In order to use Kythe's compilation extractors, they must be given precise
information about how a compilation is processed.  Currently, Kythe has
[built-in support]({{site.data.development.source_browser}}/kythe/extractors/bazel/extract.sh)
for Bazel and rudimentary support for
[CMake]({{site.data.development.source_browser}}/kythe/extractors/cmake/).
Contributing support for more build systems like [Gradle](https://gradle.org)
will greatly help the ease of use for Kythe and increase the breadth of what it
can index.

### User Interfaces

Kythe emits a lot of data and there are *many* ways to interpret/display it all.
Kythe has provided a
[sample UI]({{site.baseuri}}/examples#visualizing-cross-references), but it
currently only scratches the surface of Kythe's data.  Other ideas for
visualizers include an interactive graph, a documentation browser and a source file
hierarchical overview.
