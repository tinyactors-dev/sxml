<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="text"/>

  <xsl:template match="/">
    <xsl:for-each-group select="//reference" group-by="../name">
      <xsl:text>{current-grouping-key()}&#10;</xsl:text>
      <xsl:for-each select="current-group()">
        <!-- The newest first; references of the same year by anchor. -->
        <xsl:sort select="front/date/@year" data-type="number" order="descending"/>
        <xsl:sort select="@anchor"/>
        <xsl:text>  [{@anchor}] {front/date/@year} {front/title}&#10;</xsl:text>
      </xsl:for-each>
    </xsl:for-each-group>
  </xsl:template>
</xsl:stylesheet>
