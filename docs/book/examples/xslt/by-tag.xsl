<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="text"/>

  <xsl:template match="/">
    <xsl:for-each-group select="//book" group-by="tags/tag">
      <xsl:sort select="current-grouping-key()"/>
      <xsl:text>{current-grouping-key()}: {current-group()/@id => string-join(', ')}&#10;</xsl:text>
    </xsl:for-each-group>
  </xsl:template>
</xsl:stylesheet>
