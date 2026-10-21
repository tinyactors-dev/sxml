<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xsl:output method="text"/>

  <!-- Two archives, each in the order of time, read as one: every trace
       of both, in the order they started. -->
  <xsl:template name="xsl:initial-template">
    <xsl:merge>
      <xsl:merge-source name="first" for-each-source="'archive.xml'"
          select="traces/trace" streamable="yes">
        <xsl:merge-key select="xs:dateTime(@start)"/>
      </xsl:merge-source>
      <xsl:merge-source name="second" for-each-source="'archive-2.xml'"
          select="traces/trace" streamable="yes">
        <xsl:merge-key select="xs:dateTime(@start)"/>
      </xsl:merge-source>
      <xsl:merge-action>
        <xsl:variable name="archive"
            select="if (exists(current-merge-group('first'))) then 'first' else 'second'"/>
        <xsl:value-of select="current-merge-key(), $archive, current-merge-group()/@route"/>
        <xsl:text>&#10;</xsl:text>
      </xsl:merge-action>
    </xsl:merge>
  </xsl:template>
</xsl:stylesheet>
