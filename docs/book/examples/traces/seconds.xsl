<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xsl:mode streamable="yes"/>
  <xsl:output method="text"/>

  <!-- The archive is in the order of time: the requests of each second
       are neighbours. Copy each second's requests, a small tree, and
       compute from the copy as often as needed. -->
  <xsl:template match="/traces">
    <xsl:for-each-group select="trace" group-adjacent="substring(@start, 1, 19)">
      <xsl:variable name="second" select="copy-of(current-group())"/>
      <xsl:text>{current-grouping-key()}: {count($second)} requests, the slowest {max($second/span/@duration)} ms&#10;</xsl:text>
    </xsl:for-each-group>
  </xsl:template>
</xsl:stylesheet>
