# Inverted Index
From [On Inverted Index Compression for Search Engine Efficiency](http://www.dcs.gla.ac.uk/%7Ecraigm/publications/catena14compression.pdf):  
For each unique indexed term, the inverted index contains a `posting list`, where each _posting_ contains
occurrences information (e.g frequencies, and positions) for documents that contain the term.
To rank the documents in response to a query the posting lists for the terms of the query must be traversed, which
can be costly, especially for long posting lists.
Different ordering of the postings in the posting lists change both the algorithm for ranked retrieval and the underlying repr. of
postings in the inversed index, such as how they are compressed. 
For instance, the postings for each term can be sorted in order of impact, allowing ranked retrieval to be short-circuited once
enough documents have been retrieved. See for [example](https://blog.twitter.com/2010/twitters-new-search-architecture) where Twitter created a special codec where the posting list is ordered by *ascending* document ID, because they want to consider the most recent tweets first and abort earily as soon as they collect K of them.
However, search egines repeatedly use the traditional static `docid` ordering, where each posting list is ordered by
ascending document id(monotonically increasing), which permits a reduced inverted index size and efficient retrieval.